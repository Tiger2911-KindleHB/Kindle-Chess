#include <gtk/gtk.h>
#include <cairo.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

// KindleChess: native GTK2 chess app for jailbroken Kindle devices.
// Features: legal chess, save/resume, human-vs-human, human-vs-local-UCI-engine.
// Stockfish is not bundled. Put a Kindle-compatible UCI engine at bin/stockfish.

static bool in_bounds(int r, int f) { return r >= 0 && r < 8 && f >= 0 && f < 8; }
static int sq(int r, int f) { return r * 8 + f; }
static int row_of(int s) { return s / 8; }
static int file_of(int s) { return s % 8; }
static bool is_white_piece(char p) { return p >= 'A' && p <= 'Z'; }
static bool is_black_piece(char p) { return p >= 'a' && p <= 'z'; }
static bool is_empty(char p) { return p == '.'; }
static bool same_color(char a, char b) {
    if (is_empty(a) || is_empty(b)) return false;
    return is_white_piece(a) == is_white_piece(b);
}
static char piece_type(char p) { return (char)std::tolower((unsigned char)p); }
static std::string square_name(int s) {
    std::string out;
    out.push_back((char)('a' + file_of(s)));
    out.push_back((char)('8' - row_of(s)));
    return out;
}
static int parse_square(const std::string& s) {
    if (s.size() < 2) return -1;
    int f = s[0] - 'a';
    int r = '8' - s[1];
    if (!in_bounds(r, f)) return -1;
    return sq(r, f);
}
static std::string home_dir() {
    const char* h = std::getenv("KINDLECHESS_HOME");
    if (h && *h) return h;
    return "/mnt/us/extensions/kindlechess";
}
static std::string save_path() { return home_dir() + "/data/save.txt"; }

struct Move {
    int from = -1;
    int to = -1;
    char promotion = 0;      // lowercase q/r/b/n, 0 otherwise
    bool en_passant = false;
    bool castle = false;
};

struct Position {
    std::array<char, 64> b{};
    bool white_to_move = true;
    bool castle_wk = true, castle_wq = true, castle_bk = true, castle_bq = true;
    int ep_square = -1;
    int halfmove_clock = 0;
    int fullmove_number = 1;
    std::vector<std::string> uci_moves;
};

class ChessGame {
public:
    ChessGame() { reset(); }

    void reset() {
        pos = Position();
        const char* start = "rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR";
        for (int i = 0; i < 64; ++i) pos.b[i] = start[i];
        history.clear();
        message = "New game. White to move.";
    }

    const Position& state() const { return pos; }
    const std::string& status() const { return message; }
    const std::vector<std::string>& uciMoves() const { return pos.uci_moves; }
    bool whiteToMove() const { return pos.white_to_move; }
    char pieceAt(int s) const { return pos.b[s]; }

    std::string fen() const {
        std::ostringstream os;
        for (int r = 0; r < 8; ++r) {
            int empty = 0;
            for (int f = 0; f < 8; ++f) {
                char p = pos.b[sq(r, f)];
                if (p == '.') empty++;
                else {
                    if (empty) { os << empty; empty = 0; }
                    os << p;
                }
            }
            if (empty) os << empty;
            if (r != 7) os << '/';
        }
        os << (pos.white_to_move ? " w " : " b ");
        std::string c;
        if (pos.castle_wk) c += 'K';
        if (pos.castle_wq) c += 'Q';
        if (pos.castle_bk) c += 'k';
        if (pos.castle_bq) c += 'q';
        os << (c.empty() ? "-" : c) << ' ';
        os << (pos.ep_square >= 0 ? square_name(pos.ep_square) : "-");
        os << ' ' << pos.halfmove_clock << ' ' << pos.fullmove_number;
        return os.str();
    }

    std::vector<Move> legalMoves() const {
        std::vector<Move> pseudos;
        generatePseudo(pseudos);
        std::vector<Move> legal;
        legal.reserve(pseudos.size());
        bool movingWhite = pos.white_to_move;
        for (const Move& m : pseudos) {
            ChessGame copy = *this;
            copy.applyNoHistory(m);
            int k = copy.kingSquare(movingWhite);
            if (k >= 0 && !copy.isSquareAttacked(k, !movingWhite)) legal.push_back(m);
        }
        return legal;
    }

    std::vector<Move> legalMovesFrom(int from) const {
        std::vector<Move> out;
        for (const auto& m : legalMoves()) if (m.from == from) out.push_back(m);
        return out;
    }

    bool needsPromotionChoice(int from, int to) const {
        for (const auto& m : legalMoves()) {
            if (m.from == from && m.to == to && m.promotion) return true;
        }
        return false;
    }

    bool makeMove(int from, int to, char promotion = 0) {
        promotion = (char)std::tolower((unsigned char)promotion);
        auto moves = legalMoves();
        for (const auto& m : moves) {
            if (m.from != from || m.to != to) continue;
            if (m.promotion) {
                char want = promotion ? promotion : 'q';
                if (m.promotion != want) continue;
            }
            history.push_back(pos);
            std::string uci = square_name(m.from) + square_name(m.to);
            if (m.promotion) uci.push_back(m.promotion);
            applyNoHistory(m);
            pos.uci_moves.push_back(uci);
            updateStatus();
            return true;
        }
        message = "Illegal move.";
        return false;
    }

    bool makeUciMove(const std::string& uci) {
        if (uci.size() < 4) return false;
        int from = parse_square(uci.substr(0, 2));
        int to = parse_square(uci.substr(2, 2));
        char promo = uci.size() >= 5 ? uci[4] : 0;
        return makeMove(from, to, promo);
    }

    bool undo() {
        if (history.empty()) {
            message = "Nothing to undo.";
            return false;
        }
        pos = history.back();
        history.pop_back();
        updateStatus();
        return true;
    }

    void save() const {
        std::string dir = home_dir() + "/data";
        mkdir(dir.c_str(), 0755);
        std::ofstream f(save_path().c_str());
        if (!f) return;
        f << "KINDLECHESS 1\n";
        f << "moves=";
        for (size_t i = 0; i < pos.uci_moves.size(); ++i) {
            if (i) f << ' ';
            f << pos.uci_moves[i];
        }
        f << "\n";
    }

    bool load() {
        std::ifstream f(save_path().c_str());
        if (!f) return false;
        std::string line, movesLine;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.rfind("moves=", 0) == 0) movesLine = line.substr(6);
        }
        reset();
        std::istringstream ss(movesLine);
        std::string mv;
        bool any = false;
        while (ss >> mv) {
            if (!makeUciMove(mv)) {
                message = "Save file had an invalid move. Started a new game.";
                reset();
                return false;
            }
            any = true;
        }
        history.clear();
        if (any) message = "Resumed saved game.";
        return any;
    }

private:
    Position pos;
    std::vector<Position> history;
    std::string message;

    bool enemyAt(int s, bool white) const {
        char p = pos.b[s];
        if (p == '.') return false;
        return is_white_piece(p) != white;
    }

    int kingSquare(bool white) const {
        char k = white ? 'K' : 'k';
        for (int i = 0; i < 64; ++i) if (pos.b[i] == k) return i;
        return -1;
    }

    bool isSquareAttacked(int target, bool byWhite) const {
        int r = row_of(target), f = file_of(target);

        // Pawns attacking target.
        if (byWhite) {
            int pr = r + 1;
            if (pr < 8) {
                if (f > 0 && pos.b[sq(pr, f - 1)] == 'P') return true;
                if (f < 7 && pos.b[sq(pr, f + 1)] == 'P') return true;
            }
        } else {
            int pr = r - 1;
            if (pr >= 0) {
                if (f > 0 && pos.b[sq(pr, f - 1)] == 'p') return true;
                if (f < 7 && pos.b[sq(pr, f + 1)] == 'p') return true;
            }
        }

        static const int knight[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
        for (auto& d : knight) {
            int rr = r + d[0], ff = f + d[1];
            if (!in_bounds(rr, ff)) continue;
            char p = pos.b[sq(rr, ff)];
            if (p == (byWhite ? 'N' : 'n')) return true;
        }

        static const int king[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
        for (auto& d : king) {
            int rr = r + d[0], ff = f + d[1];
            if (!in_bounds(rr, ff)) continue;
            char p = pos.b[sq(rr, ff)];
            if (p == (byWhite ? 'K' : 'k')) return true;
        }

        static const int bishop[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
        for (auto& d : bishop) {
            int rr = r + d[0], ff = f + d[1];
            while (in_bounds(rr, ff)) {
                char p = pos.b[sq(rr, ff)];
                if (p != '.') {
                    if (is_white_piece(p) == byWhite && (piece_type(p) == 'b' || piece_type(p) == 'q')) return true;
                    break;
                }
                rr += d[0]; ff += d[1];
            }
        }

        static const int rook[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
        for (auto& d : rook) {
            int rr = r + d[0], ff = f + d[1];
            while (in_bounds(rr, ff)) {
                char p = pos.b[sq(rr, ff)];
                if (p != '.') {
                    if (is_white_piece(p) == byWhite && (piece_type(p) == 'r' || piece_type(p) == 'q')) return true;
                    break;
                }
                rr += d[0]; ff += d[1];
            }
        }
        return false;
    }

    void addMove(std::vector<Move>& out, int from, int to, char promo = 0, bool ep = false, bool castle = false) const {
        Move m; m.from = from; m.to = to; m.promotion = promo; m.en_passant = ep; m.castle = castle;
        out.push_back(m);
    }

    void addSlide(std::vector<Move>& out, int from, const int dirs[][2], int n, bool white) const {
        int r = row_of(from), f = file_of(from);
        for (int i = 0; i < n; ++i) {
            int rr = r + dirs[i][0], ff = f + dirs[i][1];
            while (in_bounds(rr, ff)) {
                int to = sq(rr, ff);
                char p = pos.b[to];
                if (p == '.') addMove(out, from, to);
                else {
                    if (is_white_piece(p) != white) addMove(out, from, to);
                    break;
                }
                rr += dirs[i][0]; ff += dirs[i][1];
            }
        }
    }

    void generatePseudo(std::vector<Move>& out) const {
        bool white = pos.white_to_move;
        for (int from = 0; from < 64; ++from) {
            char p = pos.b[from];
            if (p == '.' || is_white_piece(p) != white) continue;
            int r = row_of(from), f = file_of(from);
            char t = piece_type(p);
            if (t == 'p') {
                int dir = white ? -1 : 1;
                int startRow = white ? 6 : 1;
                int promRow = white ? 0 : 7;
                int oneR = r + dir;
                if (in_bounds(oneR, f) && pos.b[sq(oneR, f)] == '.') {
                    int to = sq(oneR, f);
                    if (oneR == promRow) {
                        addMove(out, from, to, 'q'); addMove(out, from, to, 'r');
                        addMove(out, from, to, 'b'); addMove(out, from, to, 'n');
                    } else {
                        addMove(out, from, to);
                        int twoR = r + 2 * dir;
                        if (r == startRow && in_bounds(twoR, f) && pos.b[sq(twoR, f)] == '.') addMove(out, from, sq(twoR, f));
                    }
                }
                for (int df : {-1, 1}) {
                    int cr = r + dir, cf = f + df;
                    if (!in_bounds(cr, cf)) continue;
                    int to = sq(cr, cf);
                    if (enemyAt(to, white) || to == pos.ep_square) {
                        bool ep = (to == pos.ep_square && pos.b[to] == '.');
                        if (cr == promRow) {
                            addMove(out, from, to, 'q', ep); addMove(out, from, to, 'r', ep);
                            addMove(out, from, to, 'b', ep); addMove(out, from, to, 'n', ep);
                        } else addMove(out, from, to, 0, ep);
                    }
                }
            } else if (t == 'n') {
                static const int k[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
                for (auto& d : k) {
                    int rr = r + d[0], ff = f + d[1];
                    if (!in_bounds(rr, ff)) continue;
                    int to = sq(rr, ff);
                    if (pos.b[to] == '.' || is_white_piece(pos.b[to]) != white) addMove(out, from, to);
                }
            } else if (t == 'b') {
                static const int d[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
                addSlide(out, from, d, 4, white);
            } else if (t == 'r') {
                static const int d[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                addSlide(out, from, d, 4, white);
            } else if (t == 'q') {
                static const int d[8][2] = {{-1,-1},{-1,1},{1,-1},{1,1},{-1,0},{1,0},{0,-1},{0,1}};
                addSlide(out, from, d, 8, white);
            } else if (t == 'k') {
                static const int k[8][2] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
                for (auto& d : k) {
                    int rr = r + d[0], ff = f + d[1];
                    if (!in_bounds(rr, ff)) continue;
                    int to = sq(rr, ff);
                    if (pos.b[to] == '.' || is_white_piece(pos.b[to]) != white) addMove(out, from, to);
                }
                if (white) {
                    int e1 = sq(7,4), f1 = sq(7,5), g1 = sq(7,6), d1 = sq(7,3), c1 = sq(7,2), b1 = sq(7,1);
                    if (from == e1 && pos.castle_wk && pos.b[f1] == '.' && pos.b[g1] == '.' &&
                        !isSquareAttacked(e1, false) && !isSquareAttacked(f1, false) && !isSquareAttacked(g1, false)) addMove(out, from, g1, 0, false, true);
                    if (from == e1 && pos.castle_wq && pos.b[d1] == '.' && pos.b[c1] == '.' && pos.b[b1] == '.' &&
                        !isSquareAttacked(e1, false) && !isSquareAttacked(d1, false) && !isSquareAttacked(c1, false)) addMove(out, from, c1, 0, false, true);
                } else {
                    int e8 = sq(0,4), f8 = sq(0,5), g8 = sq(0,6), d8 = sq(0,3), c8 = sq(0,2), b8 = sq(0,1);
                    if (from == e8 && pos.castle_bk && pos.b[f8] == '.' && pos.b[g8] == '.' &&
                        !isSquareAttacked(e8, true) && !isSquareAttacked(f8, true) && !isSquareAttacked(g8, true)) addMove(out, from, g8, 0, false, true);
                    if (from == e8 && pos.castle_bq && pos.b[d8] == '.' && pos.b[c8] == '.' && pos.b[b8] == '.' &&
                        !isSquareAttacked(e8, true) && !isSquareAttacked(d8, true) && !isSquareAttacked(c8, true)) addMove(out, from, c8, 0, false, true);
                }
            }
        }
    }

    void applyNoHistory(const Move& m) {
        char moving = pos.b[m.from];
        char captured = pos.b[m.to];
        bool white = is_white_piece(moving);

        pos.ep_square = -1;
        if (piece_type(moving) == 'p' || captured != '.' || m.en_passant) pos.halfmove_clock = 0;
        else pos.halfmove_clock++;

        // Update castling rights based on moving or captured rook/king.
        if (moving == 'K') { pos.castle_wk = false; pos.castle_wq = false; }
        if (moving == 'k') { pos.castle_bk = false; pos.castle_bq = false; }
        if (m.from == sq(7,0) || m.to == sq(7,0)) pos.castle_wq = false;
        if (m.from == sq(7,7) || m.to == sq(7,7)) pos.castle_wk = false;
        if (m.from == sq(0,0) || m.to == sq(0,0)) pos.castle_bq = false;
        if (m.from == sq(0,7) || m.to == sq(0,7)) pos.castle_bk = false;

        pos.b[m.to] = moving;
        pos.b[m.from] = '.';

        if (m.en_passant) {
            int cap = white ? m.to + 8 : m.to - 8;
            if (cap >= 0 && cap < 64) pos.b[cap] = '.';
        }

        if (m.castle) {
            if (moving == 'K' && m.to == sq(7,6)) { pos.b[sq(7,5)] = 'R'; pos.b[sq(7,7)] = '.'; }
            if (moving == 'K' && m.to == sq(7,2)) { pos.b[sq(7,3)] = 'R'; pos.b[sq(7,0)] = '.'; }
            if (moving == 'k' && m.to == sq(0,6)) { pos.b[sq(0,5)] = 'r'; pos.b[sq(0,7)] = '.'; }
            if (moving == 'k' && m.to == sq(0,2)) { pos.b[sq(0,3)] = 'r'; pos.b[sq(0,0)] = '.'; }
        }

        if (m.promotion) pos.b[m.to] = white ? (char)std::toupper(m.promotion) : m.promotion;

        if (piece_type(moving) == 'p' && std::abs(row_of(m.to) - row_of(m.from)) == 2) {
            pos.ep_square = sq((row_of(m.to) + row_of(m.from)) / 2, file_of(m.from));
        }

        if (!pos.white_to_move) pos.fullmove_number++;
        pos.white_to_move = !pos.white_to_move;
    }

    void updateStatus() {
        auto moves = legalMoves();
        int k = kingSquare(pos.white_to_move);
        bool inCheck = k >= 0 && isSquareAttacked(k, !pos.white_to_move);
        if (moves.empty()) {
            if (inCheck) message = std::string(pos.white_to_move ? "White" : "Black") + " is checkmated.";
            else message = "Stalemate.";
        } else if (inCheck) {
            message = std::string(pos.white_to_move ? "White" : "Black") + " to move. Check.";
        } else {
            message = std::string(pos.white_to_move ? "White" : "Black") + " to move.";
        }
    }
};

class UciEngine {
public:
    ~UciEngine() { stop(); }

    bool available(const std::string& path) const { return access(path.c_str(), X_OK) == 0; }

    bool ensureStarted(const std::string& path, std::string& err) {
        if (child > 0) return true;
        if (!available(path)) {
            err = "No executable UCI engine found at " + path;
            return false;
        }
        int inpipe[2], outpipe[2];
        if (pipe(inpipe) != 0 || pipe(outpipe) != 0) { err = "pipe() failed"; return false; }
        child = fork();
        if (child == 0) {
            dup2(inpipe[0], STDIN_FILENO);
            dup2(outpipe[1], STDOUT_FILENO);
            dup2(outpipe[1], STDERR_FILENO);
            close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]);
            execl(path.c_str(), path.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(inpipe[0]); close(outpipe[1]);
        write_fd = inpipe[1]; read_fd = outpipe[0];
        if (child < 0) { err = "fork() failed"; return false; }

        sendLine("uci");
        if (!waitForToken("uciok", 4000)) { err = "Engine did not answer uciok."; stop(); return false; }
        sendLine("setoption name Threads value 1");
        sendLine("setoption name Hash value 16");
        sendLine("setoption name Ponder value false");
        sendLine("setoption name MultiPV value 1");
        sendLine("isready");
        if (!waitForToken("readyok", 4000)) { err = "Engine did not answer readyok."; stop(); return false; }
        sendLine("ucinewgame");
        sendLine("isready");
        waitForToken("readyok", 4000);
        return true;
    }

    std::string bestMove(const std::string& path, const std::vector<std::string>& moves, int movetimeMs, std::string& err) {
        if (!ensureStarted(path, err)) return "";
        std::ostringstream poscmd;
        poscmd << "position startpos";
        if (!moves.empty()) {
            poscmd << " moves";
            for (const auto& m : moves) poscmd << ' ' << m;
        }
        sendLine(poscmd.str());
        sendLine("go movetime " + std::to_string(movetimeMs));
        std::string line;
        int timeout = std::max(3000, movetimeMs + 8000);
        long start = nowMs();
        while (nowMs() - start < timeout) {
            if (!readLine(line, 250)) continue;
            if (line.rfind("bestmove ", 0) == 0) {
                std::istringstream ss(line);
                std::string tag, mv;
                ss >> tag >> mv;
                return mv == "(none)" ? "" : mv;
            }
        }
        err = "Engine timed out waiting for bestmove.";
        return "";
    }

    void stop() {
        if (child > 0) {
            sendLine("quit");
            usleep(100000);
            kill(child, SIGTERM);
        }
        if (write_fd >= 0) close(write_fd);
        if (read_fd >= 0) close(read_fd);
        child = -1; write_fd = -1; read_fd = -1; partial.clear();
    }

private:
    pid_t child = -1;
    int write_fd = -1;
    int read_fd = -1;
    std::string partial;

    static long nowMs() {
        struct timeval tv{};
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
    }

    bool sendLine(const std::string& s) {
        if (write_fd < 0) return false;
        std::string out = s + "\n";
        return write(write_fd, out.c_str(), out.size()) == (ssize_t)out.size();
    }

    bool readLine(std::string& line, int timeoutMs) {
        line.clear();
        long end = nowMs() + timeoutMs;
        while (nowMs() < end) {
            fd_set set; FD_ZERO(&set); FD_SET(read_fd, &set);
            long remain = end - nowMs();
            struct timeval tv{}; tv.tv_sec = remain / 1000; tv.tv_usec = (remain % 1000) * 1000;
            int r = select(read_fd + 1, &set, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            char c;
            ssize_t n = read(read_fd, &c, 1);
            if (n <= 0) return false;
            if (c == '\n') { line = partial; partial.clear(); return true; }
            if (c != '\r') partial.push_back(c);
        }
        return false;
    }

    bool waitForToken(const std::string& token, int timeoutMs) {
        std::string line;
        long start = nowMs();
        while (nowMs() - start < timeoutMs) {
            if (readLine(line, 250) && line.find(token) != std::string::npos) return true;
        }
        return false;
    }
};

struct RectButton {
    std::string label;
    int x = 0, y = 0, w = 0, h = 0;
};

enum class EngineMode { Off, Black, White };

struct App {
    GtkWidget* window = nullptr;
    GtkWidget* area = nullptr;
    ChessGame game;
    UciEngine engine;
    std::vector<RectButton> buttons;
    int selected = -1;
    bool flipped = false;
    EngineMode engineMode = EngineMode::Off;
    int levelIndex = 1;
    std::vector<int> movetimes{250, 750, 1500, 3000};
    std::string uiStatus;
    int pendingFrom = -1;
    int pendingTo = -1;
};

static App app;

static std::string engine_path() {
    const char* e = std::getenv("KINDLECHESS_ENGINE");
    if (e && *e) return e;
    return home_dir() + "/bin/stockfish";
}

static bool engineShouldMove() {
    if (app.engineMode == EngineMode::Off) return false;
    if (app.game.whiteToMove() && app.engineMode == EngineMode::White) return true;
    if (!app.game.whiteToMove() && app.engineMode == EngineMode::Black) return true;
    return false;
}

static std::string engineModeLabel() {
    if (app.engineMode == EngineMode::Off) return "Engine: Off";
    if (app.engineMode == EngineMode::Black) return "Engine: Black";
    return "Engine: White";
}

static std::string pieceLetter(char p) {
    switch (piece_type(p)) {
        case 'k': return "K";
        case 'q': return "Q";
        case 'r': return "R";
        case 'b': return "B";
        case 'n': return "N";
        case 'p': return "P";
    }
    return "";
}

static void drawTextCentered(cairo_t* cr, const std::string& text, double x, double y, double w, double h, double size, bool bold=false) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);
    double tx = x + (w - ext.width) / 2.0 - ext.x_bearing;
    double ty = y + (h - ext.height) / 2.0 - ext.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, text.c_str());
}

static gboolean on_draw(GtkWidget* widget, GdkEventExpose*, gpointer) {
    cairo_t* cr = gdk_cairo_create(widget->window);
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    int W = a.width, H = a.height;
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.90);
    cairo_paint(cr);

    app.buttons.clear();
    int topH = 58;
    int statusH = 56;
    int margin = 8;
    std::vector<std::string> labels = {"New", "Undo", "Flip", engineModeLabel(), "Level: " + std::to_string(app.movetimes[app.levelIndex]) + "ms"};
    int bx = margin;
    for (const auto& label : labels) {
        int bw = (label.size() > 9) ? 150 : 78;
        if (label.rfind("Level", 0) == 0) bw = 140;
        RectButton b{label, bx, margin, bw, topH - 2 * margin};
        app.buttons.push_back(b);
        cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
        cairo_rectangle(cr, b.x, b.y, b.w, b.h);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        drawTextCentered(cr, label, b.x, b.y, b.w, b.h, 16, true);
        bx += bw + 8;
    }

    int boardMax = std::min(W - 2 * margin, H - topH - statusH - 2 * margin);
    int board = boardMax - (boardMax % 8);
    if (board < 240) board = boardMax;
    int boardX = (W - board) / 2;
    int boardY = topH + ((H - topH - statusH - board) / 2);
    int cell = board / 8;

    std::vector<Move> selectedMoves;
    if (app.selected >= 0) selectedMoves = app.game.legalMovesFrom(app.selected);

    for (int vr = 0; vr < 8; ++vr) {
        for (int vf = 0; vf < 8; ++vf) {
            int br = app.flipped ? 7 - vr : vr;
            int bf = app.flipped ? 7 - vf : vf;
            int s = sq(br, bf);
            bool light = ((br + bf) % 2 == 0);
            if (light) cairo_set_source_rgb(cr, 0.82, 0.82, 0.78);
            else cairo_set_source_rgb(cr, 0.55, 0.55, 0.52);
            cairo_rectangle(cr, boardX + vf * cell, boardY + vr * cell, cell, cell);
            cairo_fill(cr);

            if (s == app.selected) {
                cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                cairo_set_line_width(cr, 4.0);
                cairo_rectangle(cr, boardX + vf * cell + 3, boardY + vr * cell + 3, cell - 6, cell - 6);
                cairo_stroke(cr);
                cairo_set_line_width(cr, 1.0);
            }

            bool isTarget = false;
            for (const auto& m : selectedMoves) if (m.to == s) { isTarget = true; break; }
            if (isTarget) {
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
                cairo_arc(cr, boardX + vf * cell + cell / 2.0, boardY + vr * cell + cell / 2.0, cell * 0.12, 0, 6.28318);
                cairo_fill(cr);
            }

            char p = app.game.pieceAt(s);
            if (p != '.') {
                double cx = boardX + vf * cell + cell / 2.0;
                double cy = boardY + vr * cell + cell / 2.0;
                double rad = cell * 0.36;
                if (is_white_piece(p)) {
                    cairo_set_source_rgb(cr, 0.96, 0.96, 0.92);
                    cairo_arc(cr, cx, cy, rad, 0, 6.28318);
                    cairo_fill_preserve(cr);
                    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                    cairo_set_line_width(cr, 2.0);
                    cairo_stroke(cr);
                    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                } else {
                    cairo_set_source_rgb(cr, 0.03, 0.03, 0.03);
                    cairo_arc(cr, cx, cy, rad, 0, 6.28318);
                    cairo_fill(cr);
                    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                }
                drawTextCentered(cr, pieceLetter(p), boardX + vf * cell, boardY + vr * cell + 2, cell, cell, cell * 0.46, true);
            }
        }
    }

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, boardX, boardY, board, board);
    cairo_stroke(cr);

    std::string status = app.uiStatus.empty() ? app.game.status() : app.uiStatus;
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    drawTextCentered(cr, status, margin, H - statusH + 2, W - 2 * margin, statusH / 2.0, 18, true);
    std::string fen = app.game.fen();
    if (fen.size() > 92) fen = fen.substr(0, 92) + "...";
    drawTextCentered(cr, fen, margin, H - statusH / 2.0, W - 2 * margin, statusH / 2.0, 11, false);

    if (app.pendingFrom >= 0) {
        double ox = boardX + board * 0.1;
        double oy = boardY + board * 0.36;
        double ow = board * 0.8;
        double oh = board * 0.28;
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.90);
        cairo_rectangle(cr, ox, oy, ow, oh);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_set_line_width(cr, 3.0);
        cairo_stroke(cr);
        drawTextCentered(cr, "Promote to", ox, oy + 8, ow, 30, 18, true);
        const char* opts[4] = {"Q", "R", "B", "N"};
        for (int i = 0; i < 4; ++i) {
            double x = ox + 18 + i * (ow - 36) / 4.0;
            double y = oy + 48;
            double w = (ow - 52) / 4.0;
            double h = oh - 66;
            cairo_rectangle(cr, x, y, w, h);
            cairo_stroke(cr);
            drawTextCentered(cr, opts[i], x, y, w, h, 30, true);
        }
    }

    cairo_destroy(cr);
    return FALSE;
}

static void flush_gui() {
    if (app.area) gtk_widget_queue_draw(app.area);
    while (gtk_events_pending()) gtk_main_iteration();
}

static void maybeEngineMove() {
    if (!engineShouldMove()) return;
    auto legal = app.game.legalMoves();
    if (legal.empty()) return;
    int mt = app.movetimes[app.levelIndex];
    app.uiStatus = "Engine thinking...";
    flush_gui();
    std::string err;
    std::string mv = app.engine.bestMove(engine_path(), app.game.uciMoves(), mt, err);
    app.uiStatus.clear();
    if (mv.empty()) {
        app.uiStatus = err.empty() ? "Engine produced no move." : err;
    } else if (!app.game.makeUciMove(mv)) {
        app.uiStatus = "Engine sent illegal move: " + mv;
    } else {
        app.game.save();
    }
    if (app.area) gtk_widget_queue_draw(app.area);
}

static int boardSquareFromXY(GtkWidget* widget, int x, int y) {
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    int W = a.width, H = a.height;
    int topH = 58, statusH = 56, margin = 8;
    int boardMax = std::min(W - 2 * margin, H - topH - statusH - 2 * margin);
    int board = boardMax - (boardMax % 8);
    if (board < 240) board = boardMax;
    int boardX = (W - board) / 2;
    int boardY = topH + ((H - topH - statusH - board) / 2);
    if (x < boardX || x >= boardX + board || y < boardY || y >= boardY + board) return -1;
    int cell = board / 8;
    int vf = (x - boardX) / cell;
    int vr = (y - boardY) / cell;
    int br = app.flipped ? 7 - vr : vr;
    int bf = app.flipped ? 7 - vf : vf;
    return sq(br, bf);
}

static bool handlePromotionTap(GtkWidget* widget, int x, int y) {
    if (app.pendingFrom < 0) return false;
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    int W = a.width, H = a.height;
    int topH = 58, statusH = 56, margin = 8;
    int boardMax = std::min(W - 2 * margin, H - topH - statusH - 2 * margin);
    int board = boardMax - (boardMax % 8);
    if (board < 240) board = boardMax;
    int boardX = (W - board) / 2;
    int boardY = topH + ((H - topH - statusH - board) / 2);
    double ox = boardX + board * 0.1;
    double oy = boardY + board * 0.36;
    double ow = board * 0.8;
    double oh = board * 0.28;
    const char promos[4] = {'q','r','b','n'};
    for (int i = 0; i < 4; ++i) {
        double bx = ox + 18 + i * (ow - 36) / 4.0;
        double by = oy + 48;
        double bw = (ow - 52) / 4.0;
        double bh = oh - 66;
        if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
            app.game.makeMove(app.pendingFrom, app.pendingTo, promos[i]);
            app.game.save();
            app.pendingFrom = app.pendingTo = -1;
            app.selected = -1;
            maybeEngineMove();
            gtk_widget_queue_draw(app.area);
            return true;
        }
    }
    return true;
}

static void clickButton(const std::string& label) {
    app.uiStatus.clear();
    if (label == "New") {
        app.game.reset(); app.selected = -1; app.pendingFrom = app.pendingTo = -1; app.game.save();
    } else if (label == "Undo") {
        app.game.undo(); app.game.save(); app.selected = -1;
        if (engineShouldMove()) { app.game.undo(); app.game.save(); }
    } else if (label == "Flip") {
        app.flipped = !app.flipped;
    } else if (label.rfind("Engine", 0) == 0) {
        if (app.engineMode == EngineMode::Off) app.engineMode = EngineMode::Black;
        else if (app.engineMode == EngineMode::Black) app.engineMode = EngineMode::White;
        else app.engineMode = EngineMode::Off;
    } else if (label.rfind("Level", 0) == 0) {
        app.levelIndex = (app.levelIndex + 1) % (int)app.movetimes.size();
    }
    gtk_widget_queue_draw(app.area);
    maybeEngineMove();
}

static gboolean on_button(GtkWidget* widget, GdkEventButton* ev, gpointer) {
    if (ev->button != 1) return FALSE;
    int x = (int)ev->x, y = (int)ev->y;
    if (handlePromotionTap(widget, x, y)) return TRUE;
    for (const auto& b : app.buttons) {
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) {
            clickButton(b.label);
            return TRUE;
        }
    }
    if (engineShouldMove()) return TRUE;
    int s = boardSquareFromXY(widget, x, y);
    if (s < 0) return FALSE;
    char p = app.game.pieceAt(s);
    bool sideWhite = app.game.whiteToMove();
    if (app.selected < 0) {
        if (p != '.' && is_white_piece(p) == sideWhite) app.selected = s;
    } else {
        if (s == app.selected) app.selected = -1;
        else if (p != '.' && is_white_piece(p) == sideWhite) app.selected = s;
        else {
            if (app.game.needsPromotionChoice(app.selected, s)) {
                app.pendingFrom = app.selected;
                app.pendingTo = s;
            } else if (app.game.makeMove(app.selected, s)) {
                app.game.save();
                app.selected = -1;
                gtk_widget_queue_draw(app.area);
                maybeEngineMove();
            }
        }
    }
    gtk_widget_queue_draw(app.area);
    return TRUE;
}

static gboolean on_delete(GtkWidget*, GdkEvent*, gpointer) {
    app.game.save();
    app.engine.stop();
    gtk_main_quit();
    return TRUE;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--reset") {
            unlink(save_path().c_str());
        }
    }

    gtk_init(&argc, &argv);
    app.game.load();

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "L:A_N:application_ID:org.garske.kindlechess_PC:T");
    gtk_window_fullscreen(GTK_WINDOW(app.window));
    gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);

    app.area = gtk_drawing_area_new();
    gtk_widget_add_events(app.area, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(app.window), app.area);

    g_signal_connect(G_OBJECT(app.area), "expose-event", G_CALLBACK(on_draw), nullptr);
    g_signal_connect(G_OBJECT(app.area), "button-press-event", G_CALLBACK(on_button), nullptr);
    g_signal_connect(G_OBJECT(app.window), "delete-event", G_CALLBACK(on_delete), nullptr);
    g_signal_connect(G_OBJECT(app.window), "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
