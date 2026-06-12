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
#include <ctime>
#include <map>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
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
static std::string settings_path() { return home_dir() + "/data/settings.txt"; }
static std::string engine_log_path() { return home_dir() + "/data/engine.log"; }
static std::string games_dir() { return home_dir() + "/data/games"; }
static std::string slots_dir() { return home_dir() + "/data/slots"; }
static std::string slot_path(int n) { return slots_dir() + "/slot" + std::to_string(n) + ".txt"; }
static std::string pieces_custom_dir() { return home_dir() + "/pieces/custom"; }
static std::string pieces_default_dir() { return home_dir() + "/pieces/default"; }


static void append_engine_log(const std::string& msg) {
    std::string dir = home_dir() + "/data";
    mkdir(dir.c_str(), 0755);
    std::ofstream f(engine_log_path().c_str(), std::ios::app);
    if (!f) return;
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    f << "[" << tv.tv_sec << "." << tv.tv_usec << "] " << msg << "\n";
}

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
        saveTo(save_path());
    }

    void saveTo(const std::string& path) const {
        std::ofstream f(path.c_str());
        if (!f) return;
        f << "KINDLECHESS 1\n";
        f << "moves=";
        for (size_t i = 0; i < pos.uci_moves.size(); ++i) {
            if (i) f << ' ';
            f << pos.uci_moves[i];
        }
        f << "\n";
    }

    bool load() { return loadFrom(save_path()); }

    bool loadFrom(const std::string& path) {
        std::ifstream f(path.c_str());
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

    Position positionAfterPly(int ply) const {
        ChessGame tmp;
        tmp.reset();
        int n = clampIntLocal(ply, 0, (int)pos.uci_moves.size());
        for (int i = 0; i < n; ++i) tmp.makeUciMove(pos.uci_moves[i]);
        return tmp.pos;
    }

    std::string sanForUci(const std::string& uci) const {
        if (uci.size() < 4) return uci;
        int from = parse_square(uci.substr(0, 2));
        int to = parse_square(uci.substr(2, 2));
        char promo = uci.size() >= 5 ? (char)std::tolower((unsigned char)uci[4]) : 0;
        for (const auto& m : legalMoves()) {
            if (m.from != from || m.to != to) continue;
            if (m.promotion && promo && m.promotion != promo) continue;
            if (m.promotion && !promo) continue;
            return sanForMove(m);
        }
        return uci;
    }

    std::vector<std::string> sanMoveList() const {
        std::vector<std::string> out;
        ChessGame tmp;
        tmp.reset();
        for (const auto& u : pos.uci_moves) {
            out.push_back(tmp.sanForUci(u));
            tmp.makeUciMove(u);
        }
        return out;
    }

    std::pair<std::vector<char>, std::vector<char>> capturedPieces() const {
        std::vector<char> byWhite, byBlack;
        ChessGame tmp;
        tmp.reset();
        for (const auto& u : pos.uci_moves) {
            if (u.size() < 4) break;
            int from = parse_square(u.substr(0,2));
            int to = parse_square(u.substr(2,2));
            char promo = u.size() >= 5 ? (char)std::tolower((unsigned char)u[4]) : 0;
            bool found = false;
            for (const auto& m : tmp.legalMoves()) {
                if (m.from != from || m.to != to) continue;
                if (m.promotion && promo && m.promotion != promo) continue;
                if (m.promotion && !promo) continue;
                char moving = tmp.pos.b[m.from];
                char captured = tmp.capturedPieceForMove(m);
                if (captured != '.') {
                    if (is_white_piece(moving)) byWhite.push_back(captured);
                    else byBlack.push_back(captured);
                }
                tmp.applyNoHistory(m);
                tmp.pos.uci_moves.push_back(u);
                found = true;
                break;
            }
            if (!found) break;
        }
        sortCaptured(byWhite);
        sortCaptured(byBlack);
        return {byWhite, byBlack};
    }

    bool isInsufficientMaterial() const {
        std::vector<std::pair<char,int>> pieces;
        for (int i = 0; i < 64; ++i) {
            char p = pos.b[i];
            if (p == '.' || piece_type(p) == 'k') continue;
            pieces.push_back({p, i});
        }
        if (pieces.empty()) return true;
        if (pieces.size() == 1) {
            char t = piece_type(pieces[0].first);
            return t == 'b' || t == 'n';
        }
        bool allBishops = true;
        int color = -1;
        for (auto& pi : pieces) {
            if (piece_type(pi.first) != 'b') { allBishops = false; break; }
            int c = (row_of(pi.second) + file_of(pi.second)) & 1;
            if (color < 0) color = c;
            else if (c != color) allBishops = false;
        }
        return allBishops;
    }

    bool isFiftyMoveDraw() const { return pos.halfmove_clock >= 100; }

    bool isThreefoldRepetition() const {
        std::map<std::string, int> seen;
        ChessGame tmp;
        tmp.reset();
        seen[tmp.positionKey(tmp.pos)]++;
        for (const auto& u : pos.uci_moves) {
            if (!tmp.makeUciMove(u)) break;
            seen[tmp.positionKey(tmp.pos)]++;
            if (seen[tmp.positionKey(tmp.pos)] >= 3) return true;
        }
        return false;
    }

    bool isCheckmate() const {
        auto moves = legalMoves();
        if (!moves.empty()) return false;
        int k = kingSquare(pos.white_to_move);
        return k >= 0 && isSquareAttacked(k, !pos.white_to_move);
    }

    bool isStalemate() const {
        auto moves = legalMoves();
        if (!moves.empty()) return false;
        int k = kingSquare(pos.white_to_move);
        return k >= 0 && !isSquareAttacked(k, !pos.white_to_move);
    }

    std::string gameOverMessage() const {
        if (isCheckmate()) return std::string(pos.white_to_move ? "Black" : "White") + " wins by checkmate.";
        if (isStalemate()) return "Draw by stalemate.";
        if (isInsufficientMaterial()) return "Draw by insufficient material.";
        if (isFiftyMoveDraw()) return "Draw by fifty-move rule.";
        if (isThreefoldRepetition()) return "Draw by threefold repetition.";
        return "";
    }

    std::string resultString() const {
        if (isCheckmate()) return pos.white_to_move ? "0-1" : "1-0";
        if (isStalemate() || isInsufficientMaterial() || isFiftyMoveDraw() || isThreefoldRepetition()) return "1/2-1/2";
        return "*";
    }

private:
    Position pos;
    std::vector<Position> history;
    std::string message;

    static int clampIntLocal(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

    static int pieceSortValue(char p) {
        switch (piece_type(p)) {
            case 'q': return 0;
            case 'r': return 1;
            case 'b': return 2;
            case 'n': return 3;
            case 'p': return 4;
        }
        return 5;
    }

    static void sortCaptured(std::vector<char>& v) {
        std::sort(v.begin(), v.end(), [](char a, char b) {
            int va = pieceSortValue(a), vb = pieceSortValue(b);
            if (va != vb) return va < vb;
            return a < b;
        });
    }

    char capturedPieceForMove(const Move& m) const {
        char moving = pos.b[m.from];
        if (m.en_passant) {
            int cap = is_white_piece(moving) ? m.to + 8 : m.to - 8;
            if (cap >= 0 && cap < 64) return pos.b[cap];
            return '.';
        }
        return pos.b[m.to];
    }

    std::string positionKey(const Position& p) const {
        std::string k;
        k.reserve(90);
        for (char c : p.b) k.push_back(c);
        k.push_back(p.white_to_move ? 'w' : 'b');
        k.push_back(p.castle_wk ? 'K' : '-');
        k.push_back(p.castle_wq ? 'Q' : '-');
        k.push_back(p.castle_bk ? 'k' : '-');
        k.push_back(p.castle_bq ? 'q' : '-');
        if (p.ep_square >= 0) k += square_name(p.ep_square);
        else k += "--";
        return k;
    }

    std::string sanForMove(const Move& m) const {
        char moving = pos.b[m.from];
        bool white = is_white_piece(moving);
        char t = piece_type(moving);
        if (t == 'k' && m.castle) return m.to > m.from ? "O-O" : "O-O-O";

        bool capture = capturedPieceForMove(m) != '.';
        std::string san;
        if (t != 'p') {
            char letter = ' ';
            if (t == 'n') letter = 'N';
            else if (t == 'b') letter = 'B';
            else if (t == 'r') letter = 'R';
            else if (t == 'q') letter = 'Q';
            else if (t == 'k') letter = 'K';
            san.push_back(letter);

            bool sameFile = false, sameRank = false, needDisambig = false;
            for (const auto& other : legalMoves()) {
                if (other.from == m.from || other.to != m.to) continue;
                char op = pos.b[other.from];
                if (op == '.' || is_white_piece(op) != white || piece_type(op) != t) continue;
                needDisambig = true;
                if (file_of(other.from) == file_of(m.from)) sameFile = true;
                if (row_of(other.from) == row_of(m.from)) sameRank = true;
            }
            if (needDisambig) {
                if (!sameFile) san.push_back((char)('a' + file_of(m.from)));
                else if (!sameRank) san.push_back((char)('8' - row_of(m.from)));
                else {
                    san.push_back((char)('a' + file_of(m.from)));
                    san.push_back((char)('8' - row_of(m.from)));
                }
            }
        } else if (capture) {
            san.push_back((char)('a' + file_of(m.from)));
        }

        if (capture) san.push_back('x');
        san += square_name(m.to);
        if (m.promotion) {
            san += '=';
            san.push_back((char)std::toupper((unsigned char)m.promotion));
        }

        ChessGame after = *this;
        after.applyNoHistory(m);
        int k = after.kingSquare(after.pos.white_to_move);
        bool inCheck = k >= 0 && after.isSquareAttacked(k, !after.pos.white_to_move);
        if (inCheck) san += after.legalMoves().empty() ? "#" : "+";
        return san;
    }


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
        append_engine_log("--- engine start requested ---");
        append_engine_log("path=" + path);
        if (!available(path)) {
            err = "No executable UCI engine found at " + path;
            append_engine_log(err);
            return false;
        }
        int inpipe[2], outpipe[2];
        if (pipe(inpipe) != 0 || pipe(outpipe) != 0) { err = "pipe() failed"; append_engine_log(err); return false; }
        child = fork();
        if (child == 0) {
            dup2(inpipe[0], STDIN_FILENO);
            dup2(outpipe[1], STDOUT_FILENO);
            dup2(outpipe[1], STDERR_FILENO);
            close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]);
            execl(path.c_str(), path.c_str(), (char*)nullptr);
            dprintf(STDERR_FILENO, "exec failed for %s: errno=%d %s\n", path.c_str(), errno, strerror(errno));
            _exit(127);
        }
        close(inpipe[0]); close(outpipe[1]);
        write_fd = inpipe[1]; read_fd = outpipe[0];
        if (child < 0) { err = "fork() failed"; append_engine_log(err); return false; }
        append_engine_log("child pid=" + std::to_string((long long)child));

        sendLine("uci");
        if (!waitForToken("uciok", 20000)) { err = "Engine did not answer uciok. See data/engine.log."; append_engine_log(err); stop(); return false; }
        sendLine("setoption name Threads value 1");
        sendLine("setoption name Hash value 16");
        sendLine("setoption name Ponder value false");
        sendLine("setoption name MultiPV value 1");
        sendLine("isready");
        if (!waitForToken("readyok", 15000)) { err = "Engine did not answer readyok. See data/engine.log."; append_engine_log(err); stop(); return false; }
        sendLine("ucinewgame");
        sendLine("isready");
        waitForToken("readyok", 15000);
        append_engine_log("engine initialized successfully");
        return true;
    }

    std::string bestMove(const std::string& path, const std::vector<std::string>& moves, int movetimeMs, int targetElo, std::string& err) {
        if (!ensureStarted(path, err)) return "";
        configureStrength(targetElo);
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
            append_engine_log("<< " + line);
            if (line.rfind("bestmove ", 0) == 0) {
                std::istringstream ss(line);
                std::string tag, mv;
                ss >> tag >> mv;
                return mv == "(none)" ? "" : mv;
            }
        }
        err = "Engine timed out waiting for bestmove. See data/engine.log.";
        append_engine_log(err);
        return "";
    }

    void stop() {
        if (child > 0) {
            append_engine_log("stopping engine pid=" + std::to_string((long long)child));
            sendLine("quit");
            waitForExit(500);
            if (child > 0) {
                kill(child, SIGTERM);
                waitForExit(500);
            }
            if (child > 0) {
                kill(child, SIGKILL);
                waitForExit(500);
            }
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

    void configureStrength(int targetElo) {
        int elo = std::max(250, std::min(3000, targetElo));
        int skill = std::max(0, std::min(20, (elo - 250) * 20 / 2750));
        int uciElo = std::max(1320, std::min(3190, elo));
        sendLine("setoption name Skill Level value " + std::to_string(skill));
        sendLine("setoption name UCI_LimitStrength value true");
        sendLine("setoption name UCI_Elo value " + std::to_string(uciElo));
        sendLine("isready");
        waitForToken("readyok", 5000);
    }

    static long nowMs() {
        struct timeval tv{};
        gettimeofday(&tv, nullptr);
        return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
    }

    void waitForExit(int timeoutMs) {
        if (child <= 0) return;
        long end = nowMs() + timeoutMs;
        while (nowMs() < end) {
            int status = 0;
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) {
                append_engine_log("engine exited status=" + std::to_string(status));
                child = -1;
                return;
            }
            usleep(50000);
        }
    }

    bool sendLine(const std::string& s) {
        if (write_fd < 0) return false;
        append_engine_log(">> " + s);
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
            if (n <= 0) {
                append_engine_log("engine pipe closed while reading");
                return false;
            }
            if (c == '\n') { line = partial; partial.clear(); return true; }
            if (c != '\r') partial.push_back(c);
        }
        return false;
    }

    bool waitForToken(const std::string& token, int timeoutMs) {
        std::string line;
        long start = nowMs();
        while (nowMs() - start < timeoutMs) {
            if (readLine(line, 250)) {
                append_engine_log("<< " + line);
                if (line.find(token) != std::string::npos) return true;
            }
        }
        append_engine_log("timeout waiting for token=" + token);
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
    int engineEloIndex = 5;
    std::vector<int> engineElos{250, 500, 750, 1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750, 3000};
    std::string uiStatus;
    int pendingFrom = -1;
    int pendingTo = -1;

    bool showSettings = false;
    int settingsPage = 0; // 0 main, 1 save/load
    bool confirmNew = false;
    bool showGameOver = false;
    std::string gameOverText;
    bool resigned = false;
    std::string resignedMessage;

    bool confirmMoves = false;
    bool showMoveConfirm = false;
    int confirmFrom = -1;
    int confirmTo = -1;
    char confirmPromotion = 0;

    bool reviewMode = false;
    int reviewPly = 0;
    std::vector<RectButton> reviewButtons;

    int hintFrom = -1;
    int hintTo = -1;

    bool showCoordinates = true;
    bool showMoveList = true;
    int uiFontSize = 40;
    bool usePieceImages = true;
    bool pieceImagesLoaded = false;
    std::array<GdkPixbuf*, 128> pieceImages{};
    std::array<GdkPixbuf*, 128> pieceImagesInverted{};
    // 0 = Auto, 1 = Off, 2 = On. Used for Kindle dark-mode display inversion.
    int darkPieceMode = 0;

    std::vector<RectButton> overlayButtons;
};

static App app;

static int clampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

static int settingsFontSize() {
    // Settings text intentionally runs larger than the board/status UI.
    return clampInt(app.uiFontSize + 20, 28, 64);
}

static std::string darkPieceModeLabel() {
    if (app.darkPieceMode == 1) return "Off";
    if (app.darkPieceMode == 2) return "On";
    return "Auto";
}

static bool fileContainsDarkModeToken(const std::string& path) {
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        std::string l = line;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if ((l.find("dark") != std::string::npos || l.find("night") != std::string::npos || l.find("invert") != std::string::npos) &&
            (l.find("true") != std::string::npos || l.find("=1") != std::string::npos || l.find(":1") != std::string::npos || l.find("on") != std::string::npos)) {
            return true;
        }
    }
    return false;
}

static bool detectKindleDarkMode() {
    // Kindle firmware does not expose a stable public homebrew API for dark-mode
    // state. Check a few common preference locations and fall back to manual mode.
    const char* env = std::getenv("KINDLECHESS_DARK_MODE");
    if (env && (*env == '1' || std::strcmp(env, "true") == 0 || std::strcmp(env, "on") == 0)) return true;
    std::vector<std::string> paths{
        "/var/local/java/prefs/com.amazon.ebook.framework/prefs",
        "/var/local/java/prefs/com.amazon.ebook.booklet.reader/reader.pref",
        "/var/local/system/darkmode",
        "/var/local/system/nightmode"
    };
    for (const auto& path : paths) if (fileContainsDarkModeToken(path)) return true;
    return false;
}

static bool shouldInvertPieceImages() {
    if (app.darkPieceMode == 1) return false;
    if (app.darkPieceMode == 2) return true;
    return detectKindleDarkMode();
}

static void ensure_data_dir() {
    std::string dir = home_dir() + "/data";
    mkdir(dir.c_str(), 0755);
}

static void ensure_games_dir() {
    ensure_data_dir();
    mkdir(games_dir().c_str(), 0755);
}

static void ensure_slots_dir() {
    ensure_data_dir();
    mkdir(slots_dir().c_str(), 0755);
}

static std::string nowTimestampForFile() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char buf[64];
    if (tm) std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", tm);
    else std::snprintf(buf, sizeof(buf), "%ld", (long)t);
    return buf;
}

static std::string todayForPGN() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char buf[32];
    if (tm) std::strftime(buf, sizeof(buf), "%Y.%m.%d", tm);
    else std::snprintf(buf, sizeof(buf), "????.??.??");
    return buf;
}

static void saveAppSettings() {
    ensure_data_dir();
    std::ofstream f(settings_path().c_str());
    if (!f) return;
    f << "KINDLECHESS_SETTINGS 1\n";
    f << "ui_font_size=" << app.uiFontSize << "\n";
    f << "show_coordinates=" << (app.showCoordinates ? 1 : 0) << "\n";
    f << "show_move_list=" << (app.showMoveList ? 1 : 0) << "\n";
    f << "engine_mode=" << (app.engineMode == EngineMode::Off ? 0 : (app.engineMode == EngineMode::Black ? 1 : 2)) << "\n";
    f << "engine_elo=" << app.engineElos[app.engineEloIndex] << "\n";
    f << "flipped=0\n";
    f << "use_piece_images=" << (app.usePieceImages ? 1 : 0) << "\n";
    f << "confirm_moves=" << (app.confirmMoves ? 1 : 0) << "\n";
    f << "dark_piece_mode=" << app.darkPieceMode << "\n";
}

static void loadAppSettings() {
    std::ifstream f(settings_path().c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        int n = std::atoi(val.c_str());
        if (key == "ui_font_size") app.uiFontSize = clampInt(n, 14, 50);
        else if (key == "show_coordinates") app.showCoordinates = n != 0;
        else if (key == "show_move_list") app.showMoveList = n != 0;
        else if (key == "engine_mode") app.engineMode = (n == 1 ? EngineMode::Black : (n == 2 ? EngineMode::White : EngineMode::Off));
        else if (key == "engine_elo") {
            int best = 0;
            for (int i = 1; i < (int)app.engineElos.size(); ++i)
                if (std::abs(app.engineElos[i] - n) < std::abs(app.engineElos[best] - n)) best = i;
            app.engineEloIndex = best;
        }
        else if (key == "flipped") app.flipped = false;
        else if (key == "use_piece_images") app.usePieceImages = n != 0;
        else if (key == "confirm_moves") app.confirmMoves = n != 0;
        else if (key == "dark_piece_mode") app.darkPieceMode = clampInt(n, 0, 2);
    }
}

static std::string engine_path() {
    const char* e = std::getenv("KINDLECHESS_ENGINE");
    if (e && *e) return e;
    return home_dir() + "/bin/stockfish";
}

static bool engineShouldMove() {
    if (app.reviewMode) return false;
    if (app.showMoveConfirm) return false;
    if (app.resigned) return false;
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

static int currentEngineElo() {
    return app.engineElos[clampInt(app.engineEloIndex, 0, (int)app.engineElos.size() - 1)];
}

static int engineMovetimeForElo(int elo) {
    // The user sees Elo, not milliseconds. Internally this bounds Stockfish thinking time
    // so the Kindle remains responsive and battery-friendly.
    return clampInt(elo, 250, 3000);
}

static bool gameSetupLocked() {
    return !app.game.uciMoves().empty();
}

static bool updateGameOverPopup() {
    std::string msg = app.game.gameOverMessage();
    if (!msg.empty()) {
        app.showGameOver = true;
        app.gameOverText = msg;
        app.selected = -1;
        app.pendingFrom = app.pendingTo = -1;
        app.showMoveConfirm = false;
        return true;
    }
    app.showGameOver = false;
    app.gameOverText.clear();
    return false;
}

static std::string exportPGN() {
    ensure_games_dir();
    std::string file = games_dir() + "/game-" + nowTimestampForFile() + ".pgn";
    std::ofstream f(file.c_str());
    if (!f) return "Could not export PGN.";

    std::string result = app.game.resultString();
    f << "[Event \"KindleChess\"]\n";
    f << "[Site \"Kindle Paperwhite\"]\n";
    f << "[Date \"" << todayForPGN() << "\"]\n";
    f << "[Round \"-\"]\n";
    f << "[White \"White\"]\n";
    f << "[Black \"Black\"]\n";
    f << "[Result \"" << result << "\"]\n\n";

    auto sans = app.game.sanMoveList();
    int chars = 0;
    for (size_t i = 0; i < sans.size(); ++i) {
        std::ostringstream token;
        if (i % 2 == 0) token << (i / 2 + 1) << ". ";
        token << sans[i] << ' ';
        std::string t = token.str();
        if (chars + (int)t.size() > 78) { f << "\n"; chars = 0; }
        f << t;
        chars += (int)t.size();
    }
    if (chars + (int)result.size() + 1 > 78) f << "\n";
    f << result << "\n";
    return "PGN exported: " + file;
}

static std::string slotSummary(int n) {
    std::ifstream f(slot_path(n).c_str());
    if (!f) return "Slot " + std::to_string(n) + ": Empty";
    ChessGame tmp;
    if (!tmp.loadFrom(slot_path(n))) return "Slot " + std::to_string(n) + ": Invalid";
    std::ostringstream os;
    os << "Slot " << n << ": " << tmp.uciMoves().size() << " ply, " << (tmp.whiteToMove() ? "White" : "Black") << " to move";
    std::string over = tmp.gameOverMessage();
    if (!over.empty()) os << ", finished";
    return os.str();
}

static void saveSlot(int n) {
    ensure_slots_dir();
    app.game.saveTo(slot_path(n));
    app.uiStatus = "Saved to slot " + std::to_string(n) + ".";
}

static bool loadSlot(int n) {
    ChessGame tmp;
    if (!tmp.loadFrom(slot_path(n))) {
        app.uiStatus = "Slot " + std::to_string(n) + " is empty or invalid.";
        return false;
    }
    app.game = tmp;
    app.game.save();
    app.selected = -1;
    app.pendingFrom = app.pendingTo = -1;
    app.showMoveConfirm = false;
    app.hintFrom = app.hintTo = -1;
    app.reviewMode = false;
    app.reviewPly = 0;
    app.uiStatus = "Loaded slot " + std::to_string(n) + ".";
    updateGameOverPopup();
    return true;
}

static void deleteSlot(int n) {
    unlink(slot_path(n).c_str());
    app.uiStatus = "Deleted slot " + std::to_string(n) + ".";
}

static std::string hintMoveText() {
    if (app.hintFrom < 0 || app.hintTo < 0) return "";
    return "Hint: " + square_name(app.hintFrom) + "-" + square_name(app.hintTo);
}

static std::string pieceShortName(char p) {
    bool white = is_white_piece(p);
    char t = piece_type(p);
    std::string out;
    out.push_back(white ? 'w' : 'b');
    out.push_back(t);
    return out;
}

static std::string pieceLongName(char p) {
    std::string color = is_white_piece(p) ? "white_" : "black_";
    switch (piece_type(p)) {
        case 'k': return color + "king";
        case 'q': return color + "queen";
        case 'r': return color + "rook";
        case 'b': return color + "bishop";
        case 'n': return color + "knight";
        case 'p': return color + "pawn";
    }
    return "";
}

static bool fileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static GdkPixbuf* loadPieceFile(const std::string& path) {
    if (!fileExists(path)) return nullptr;
    GError* err = nullptr;
    GdkPixbuf* pix = gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (err) g_error_free(err);
    return pix;
}

static void freePieceImages() {
    for (auto& pix : app.pieceImages) {
        if (pix) {
            g_object_unref(G_OBJECT(pix));
            pix = nullptr;
        }
    }
    for (auto& pix : app.pieceImagesInverted) {
        if (pix) {
            g_object_unref(G_OBJECT(pix));
            pix = nullptr;
        }
    }
    app.pieceImagesLoaded = false;
}

static GdkPixbuf* makeInvertedPixbuf(GdkPixbuf* src) {
    if (!src) return nullptr;
    GdkPixbuf* dst = gdk_pixbuf_copy(src);
    if (!dst) return nullptr;
    int w = gdk_pixbuf_get_width(dst);
    int h = gdk_pixbuf_get_height(dst);
    int stride = gdk_pixbuf_get_rowstride(dst);
    int channels = gdk_pixbuf_get_n_channels(dst);
    guchar* pixels = gdk_pixbuf_get_pixels(dst);
    if (!pixels || channels < 3) return dst;
    for (int y = 0; y < h; ++y) {
        guchar* row = pixels + y * stride;
        for (int x = 0; x < w; ++x) {
            guchar* px = row + x * channels;
            px[0] = 255 - px[0];
            px[1] = 255 - px[1];
            px[2] = 255 - px[2];
        }
    }
    return dst;
}

static GdkPixbuf* tryLoadPieceImage(char p) {
    if (p == '.') return nullptr;
    std::vector<std::string> dirs{pieces_custom_dir(), pieces_default_dir()};
    std::vector<std::string> names{
        pieceShortName(p) + ".png",
        pieceLongName(p) + ".png"
    };
    for (const auto& dir : dirs) {
        for (const auto& name : names) {
            GdkPixbuf* pix = loadPieceFile(dir + "/" + name);
            if (pix) return pix;
        }
    }
    return nullptr;
}

static void loadPieceImages() {
    freePieceImages();
    const char pieces[] = {'K','Q','R','B','N','P','k','q','r','b','n','p'};
    for (char p : pieces) {
        app.pieceImages[(unsigned char)p] = tryLoadPieceImage(p);
        if (app.pieceImages[(unsigned char)p]) {
            app.pieceImagesInverted[(unsigned char)p] = makeInvertedPixbuf(app.pieceImages[(unsigned char)p]);
        }
    }
    app.pieceImagesLoaded = true;
}

static bool haveAllPieceImages() {
    if (!app.pieceImagesLoaded) loadPieceImages();
    const char pieces[] = {'K','Q','R','B','N','P','k','q','r','b','n','p'};
    for (char p : pieces) if (!app.pieceImages[(unsigned char)p]) return false;
    return true;
}

static bool drawPieceImage(cairo_t* cr, char p, double x, double y, double cell) {
    if (!app.usePieceImages) return false;
    if (!app.pieceImagesLoaded) loadPieceImages();
    GdkPixbuf* pix = shouldInvertPieceImages() ? app.pieceImagesInverted[(unsigned char)p] : app.pieceImages[(unsigned char)p];
    if (!pix) pix = app.pieceImages[(unsigned char)p];
    if (!pix) return false;
    int pw = gdk_pixbuf_get_width(pix);
    int ph = gdk_pixbuf_get_height(pix);
    if (pw <= 0 || ph <= 0) return false;
    double target = cell * 0.86;
    double scale = target / std::max(pw, ph);
    double dx = x + (cell - pw * scale) / 2.0;
    double dy = y + (cell - ph * scale) / 2.0;
    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, pix, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return true;
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

    // Kindle users may intentionally run very large UI fonts.  Keep labels inside
    // their rectangles rather than letting text spill outside buttons/popups.
    double fitted = std::max(8.0, size);
    cairo_text_extents_t ext{};
    for (;;) {
        cairo_set_font_size(cr, fitted);
        cairo_text_extents(cr, text.c_str(), &ext);
        if ((ext.width <= std::max(4.0, w - 10.0) && ext.height <= std::max(4.0, h - 6.0)) || fitted <= 8.0) break;
        fitted -= 1.0;
    }

    double tx = x + (w - ext.width) / 2.0 - ext.x_bearing;
    double ty = y + (h - ext.height) / 2.0 - ext.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, text.c_str());
}

static void drawTextLeft(cairo_t* cr, const std::string& text, double x, double y, double size, bool bold=false) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text.c_str());
}

static std::vector<std::string> toolbarLabels() {
    return {"New", "Undo", "Settings", "Exit"};
}

static int buttonWidthFor(const std::string& label) {
    int charW = std::max(10, (int)(app.uiFontSize * 0.60));
    int base = 58 + (int)label.size() * charW;
    if (label.rfind("Engine", 0) == 0) base += 26;
    if (label.rfind("Elo", 0) == 0) base += 22;
    return clampInt(base, 92, 260);
}

static int toolbarHeightFor(int W) {
    int margin = 8;
    int rowH = std::max(38, app.uiFontSize + 24);
    int rows = 1;
    int bx = margin;
    for (const auto& label : toolbarLabels()) {
        int bw = buttonWidthFor(label);
        if (bx + bw > W - margin && bx > margin) {
            rows++;
            bx = margin;
        }
        bx += bw + 8;
    }
    return margin + rows * rowH + margin;
}

struct Layout {
    int W = 0, H = 0;
    int margin = 8;
    int topH = 64;
    int statusH = 76;
    int board = 0, boardX = 0, boardY = 0, cell = 0;
    bool hasPanel = false;
    int panelX = 0, panelY = 0, panelW = 0, panelH = 0;
    int capturedH = 0;
};

static Layout computeLayout(GtkWidget* widget) {
    GtkAllocation a;
    gtk_widget_get_allocation(widget, &a);
    Layout L;
    L.W = a.width; L.H = a.height;
    L.margin = 8;
    L.topH = toolbarHeightFor(L.W);
    L.statusH = std::max(88, app.uiFontSize * 3 + 18);

    int availableW = L.W - 2 * L.margin;
    int requestedPanelW = app.showMoveList ? std::max(160, app.uiFontSize * 8) : 0;
    L.hasPanel = app.showMoveList && L.W >= 850 && requestedPanelW + 360 < availableW;

    // If there is no side panel, captured pieces live below the board.  Reserve
    // explicit vertical space for them so large UI fonts do not cause that area
    // to be clipped away by the board/status region.
    L.capturedH = L.hasPanel ? 0 : std::max(148, std::min(240, app.uiFontSize * 3 + 62));

    int availableH = L.H - L.topH - L.statusH - 2 * L.margin - L.capturedH;
    if (availableH < 240) availableH = L.H - L.topH - L.statusH - 2 * L.margin;
    int boardAreaW = availableW - (L.hasPanel ? requestedPanelW + L.margin : 0);
    int boardMax = std::min(boardAreaW, availableH);
    L.board = boardMax - (boardMax % 8);
    if (L.board < 240) L.board = boardMax;
    L.cell = std::max(1, L.board / 8);
    L.boardX = L.hasPanel ? L.margin : (L.W - L.board) / 2;
    L.boardY = L.topH + ((availableH - L.board) / 2);
    if (L.boardY < L.topH + L.margin) L.boardY = L.topH + L.margin;
    if (L.hasPanel) {
        L.panelX = L.boardX + L.board + L.margin;
        L.panelY = L.boardY;
        L.panelW = L.W - L.panelX - L.margin;
        L.panelH = L.board;
    }
    return L;
}

static std::string moveSummaryLine() {
    const auto& moves = app.game.uciMoves();
    std::ostringstream os;
    os << "Move " << (moves.size() / 2 + 1);
    if (!moves.empty()) os << "  Last: " << moves.back();
    os << "  " << engineModeLabel();
    if (app.engineMode != EngineMode::Off) os << "  Elo: " << currentEngineElo();
    return os.str();
}

static void lastMoveSquares(int& from, int& to) {
    from = -1; to = -1;
    const auto& moves = app.game.uciMoves();
    if (moves.empty()) return;
    const std::string& u = moves.back();
    if (u.size() < 4) return;
    from = parse_square(u.substr(0, 2));
    to = parse_square(u.substr(2, 2));
}

static std::vector<std::string> moveHistoryLines(int maxLines) {
    std::vector<std::string> lines;
    const auto& m = app.game.uciMoves();
    for (size_t i = 0; i < m.size(); i += 2) {
        std::ostringstream os;
        os << (i / 2 + 1) << ". " << m[i];
        if (i + 1 < m.size()) os << "  " << m[i + 1];
        lines.push_back(os.str());
    }
    if ((int)lines.size() > maxLines) lines.erase(lines.begin(), lines.begin() + ((int)lines.size() - maxLines));
    return lines;
}

static void drawButton(cairo_t* cr, const RectButton& b, double fontSize) {
    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, b.x, b.y, b.w, b.h);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    drawTextCentered(cr, b.label, b.x, b.y, b.w, b.h, fontSize, true);
}

static void drawToolbar(cairo_t* cr, int W) {
    app.buttons.clear();
    int margin = 8;
    int rowH = std::max(38, app.uiFontSize + 24);
    int bx = margin;
    int by = margin;
    for (const auto& label : toolbarLabels()) {
        int bw = buttonWidthFor(label);
        if (bx + bw > W - margin && bx > margin) {
            bx = margin;
            by += rowH;
        }
        RectButton b{label, bx, by, bw, rowH - 8};
        app.buttons.push_back(b);
        drawButton(cr, b, std::max(14, app.uiFontSize - 4));
        bx += bw + 8;
    }
}

static int capturedLabelWidth() {
    // Reserve a wider label area so captured-piece icons do not sit tight
    // against the text at large Kindle-friendly font sizes.
    return std::max(260, std::min(430, app.uiFontSize * 8));
}

static int fitCapturedCell(int availableW, int requestedCell, int maxPieces) {
    if (maxPieces <= 0) return requestedCell;
    int labelW = capturedLabelWidth();
    int usable = std::max(40, availableW - labelW - 18);
    int fit = (usable - std::max(0, maxPieces - 1) * 3) / maxPieces;
    return clampInt(std::min(requestedCell, fit), 20, requestedCell);
}

static void drawCapturedRow(cairo_t* cr, const std::string& label, const std::vector<char>& pieces, int x, int y, int w, int cell, int fs) {
    drawTextLeft(cr, label, x, y + fs + 3, fs, true);
    int px = x + capturedLabelWidth();
    int py = y;
    int maxX = x + w - cell - 4;
    for (char p : pieces) {
        if (px > maxX) break;
        if (!drawPieceImage(cr, p, px, py, cell)) {
            drawTextCentered(cr, pieceLetter(p), px, py, cell, cell, fs + 3, true);
        }
        px += cell + 3;
    }
    // Intentionally leave the row blank when nothing has been captured.
}

static void drawCapturedBox(cairo_t* cr, int x, int y, int w, int h) {
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.74);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
}

static void drawCapturedPieces(cairo_t* cr, const Layout& L) {
    auto caps = app.game.capturedPieces();
    int baseCell = std::max(32, std::min(58, app.uiFontSize + 14));
    int fs = std::max(22, std::min(38, app.uiFontSize - 2));
    int maxPieces = std::max((int)caps.first.size(), (int)caps.second.size());
    if (L.hasPanel) {
        int boxH = std::max(152, std::min(220, baseCell * 2 + fs + 42));
        int y = L.panelY + L.panelH - boxH - 8;
        if (y < L.panelY + 86) return;
        int cell = fitCapturedCell(L.panelW - 24, baseCell, maxPieces);
        int rowGap = 10;
        drawCapturedBox(cr, L.panelX + 4, y, L.panelW - 8, boxH);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        drawTextCentered(cr, "Captured", L.panelX + 8, y + 8, L.panelW - 16, fs + 8, fs, true);
        int rowY = y + fs + 24;
        drawCapturedRow(cr, "White captured", caps.first, L.panelX + 14, rowY, L.panelW - 28, cell, fs);
        drawCapturedRow(cr, "Black captured", caps.second, L.panelX + 14, rowY + cell + rowGap, L.panelW - 28, cell, fs);
    } else {
        int areaTop = L.boardY + L.board + 8;
        int areaBottom = L.H - L.statusH - 8;
        int available = areaBottom - areaTop;
        if (available <= 70) return;

        int rowGap = 10;
        int cell = std::min(baseCell, std::max(22, (available - fs - rowGap - 32) / 2));
        cell = fitCapturedCell(L.board - 24, cell, maxPieces);
        int boxH = std::min(available, std::max(132, fs + cell * 2 + rowGap + 34));
        int boxY = areaTop + std::max(0, (available - boxH) / 2);

        drawCapturedBox(cr, L.boardX, boxY, L.board, boxH);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        int y = boxY + fs + 12;
        drawCapturedRow(cr, "White captured", caps.first, L.boardX + 12, y, L.board - 24, cell, fs);
        drawCapturedRow(cr, "Black captured", caps.second, L.boardX + 12, y + cell + rowGap, L.board - 24, cell, fs);
    }
}

static void drawMovePanel(cairo_t* cr, const Layout& L) {
    if (!L.hasPanel) return;
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.84);
    cairo_rectangle(cr, L.panelX, L.panelY, L.panelW, L.panelH);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
    drawTextCentered(cr, "Moves", L.panelX, L.panelY + 6, L.panelW, 30, std::max(16, std::min(26, app.uiFontSize - 2)), true);
    int lineH = std::max(20, std::min(34, app.uiFontSize + 4));
    int capturedReserve = std::max(170, std::min(250, app.uiFontSize * 3 + 100));
    int maxLines = std::max(1, (L.panelH - 48 - capturedReserve) / lineH);
    auto lines = moveHistoryLines(maxLines);
    int y = L.panelY + 48;
    for (const auto& line : lines) {
        drawTextLeft(cr, line, L.panelX + 12, y, std::max(13, std::min(24, app.uiFontSize - 5)), false);
        y += lineH;
    }
}

static void drawCoordinates(cairo_t* cr, const Layout& L) {
    if (!app.showCoordinates || L.cell < 34) return;
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    int fs = std::max(10, app.uiFontSize - 9);
    for (int vf = 0; vf < 8; ++vf) {
        int bf = app.flipped ? 7 - vf : vf;
        std::string file(1, (char)('a' + bf));
        drawTextCentered(cr, file, L.boardX + vf * L.cell, L.boardY + L.board - fs - 2, L.cell, fs + 2, fs, true);
    }
    for (int vr = 0; vr < 8; ++vr) {
        int br = app.flipped ? 7 - vr : vr;
        std::string rank(1, (char)('8' - br));
        drawTextCentered(cr, rank, L.boardX + 2, L.boardY + vr * L.cell + 1, fs + 4, fs + 6, fs, true);
    }
}

static void drawConfirmNew(cairo_t* cr, const Layout& L) {
    app.overlayButtons.clear();
    double ow = std::min((double)L.W * 0.84, 620.0);
    double oh = 230;
    double ox = (L.W - ow) / 2.0;
    double oy = (L.H - oh) / 2.0;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.90);
    cairo_rectangle(cr, ox, oy, ow, oh);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0,0,0);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);
    drawTextCentered(cr, "Start a new game?", ox, oy + 20, ow, 42, app.uiFontSize + 2, true);
    drawTextCentered(cr, "This will replace the current saved game.", ox + 20, oy + 76, ow - 40, 36, std::max(15, app.uiFontSize - 4), false);
    int bw = std::max(210, std::min(280, (int)(ow / 2 - 28)));
    int bh = std::max(60, std::min(76, app.uiFontSize + 24));
    RectButton cancel{"Cancel", (int)(ox + ow/2 - bw - 12), (int)(oy + oh - bh - 24), bw, bh};
    RectButton yes{"New Game", (int)(ox + ow/2 + 12), (int)(oy + oh - bh - 24), bw, bh};
    app.overlayButtons.push_back(cancel); app.overlayButtons.push_back(yes);
    drawButton(cr, cancel, std::max(16, app.uiFontSize - 2));
    drawButton(cr, yes, std::max(16, app.uiFontSize - 2));
}

static void drawSettingsMain(cairo_t* cr, const Layout& L, double ox, double oy, double ow, double oh) {
    bool locked = gameSetupLocked();
    int sfs = settingsFontSize();
    drawTextCentered(cr, "Settings", ox, oy + 12, ow, std::max(56, sfs + 14), sfs + 6, true);
    drawTextCentered(cr, locked ? "Engine side and Elo are locked after the first move." : "Set engine side and Elo before making the first move.",
                     ox + 22, oy + 70, ow - 44, std::max(38, sfs - 8), std::max(22, sfs - 18), false);

    int bw = (int)((ow - 96) / 2.0);
    int bh = std::max(66, std::min(88, sfs + 22));
    int left = (int)(ox + 42);
    int right = left + bw + 12;
    int y = (int)(oy + 118);
    int gap = bh + 10;

    std::string engineLabel = std::string(locked ? "Engine Locked: " : "Engine: ");
    if (app.engineMode == EngineMode::Off) engineLabel += "Off";
    else if (app.engineMode == EngineMode::Black) engineLabel += "Black";
    else engineLabel += "White";

    RectButton engine{engineLabel, left, y, bw * 2 + 12, bh}; y += gap;
    RectButton eloDown{"Elo -250", left, y, bw, bh};
    RectButton eloUp{"Elo +250", right, y, bw, bh}; y += gap;
    RectButton eloValue{"Elo: " + std::to_string(currentEngineElo()), left, y, bw * 2 + 12, bh}; y += gap;

    RectButton hint{"Hint", left, y, bw, bh};
    RectButton restart{"Restart Engine", right, y, bw, bh}; y += gap;
    RectButton exportBtn{"Export PGN", left, y, bw, bh};
    RectButton confirm{std::string("Confirm Moves: ") + (app.confirmMoves ? "On" : "Off"), right, y, bw, bh}; y += gap;
    RectButton saves{"Save / Load", left, y, bw, bh};
    RectButton review{"Review Game", right, y, bw, bh}; y += gap;

    RectButton coords{std::string("Coordinates: ") + (app.showCoordinates ? "On" : "Off"), left, y, bw, bh};
    RectButton moves{std::string("Move List: ") + (app.showMoveList ? "On" : "Off"), right, y, bw, bh}; y += gap;
    RectButton smaller{"Font -", left, y, bw, bh};
    RectButton bigger{"Font +", right, y, bw, bh}; y += gap;
    RectButton pieceMode{std::string("Piece PNGs: ") + (app.usePieceImages ? "On" : "Off"), left, y, bw, bh};
    RectButton reloadPieces{"Reload PNGs", right, y, bw, bh}; y += gap;
    RectButton darkFix{std::string("Dark Piece Fix: ") + darkPieceModeLabel(), left, y, bw * 2 + 12, bh}; y += gap;
    RectButton close{"Close Settings", left, y, bw * 2 + 12, bh};

    app.overlayButtons = {engine, eloDown, eloUp, eloValue, hint, restart, exportBtn, confirm, saves, review,
                          coords, moves, smaller, bigger, pieceMode, reloadPieces, darkFix, close};
    double fs = sfs;
    for (const auto& b : app.overlayButtons) drawButton(cr, b, fs);
}

static void drawSaveLoadSettings(cairo_t* cr, const Layout& L, double ox, double oy, double ow, double oh) {
    int sfs = settingsFontSize();
    drawTextCentered(cr, "Save / Load Games", ox, oy + 12, ow, std::max(56, sfs + 14), sfs + 4, true);
    drawTextCentered(cr, "Five local save slots. Loading replaces the current board.", ox + 20, oy + 72, ow - 40, std::max(38, sfs - 8), std::max(22, sfs - 18), false);

    int left = (int)(ox + 30);
    int y = (int)(oy + 126);
    int rowH = std::max(92, sfs + 48);
    int smallW = std::max(88, std::min(120, sfs * 2));
    int summaryW = (int)(ow - 70 - (smallW * 3 + 24));
    int bh = std::max(62, std::min(82, sfs + 20));
    app.overlayButtons.clear();
    double fs = std::max(24, std::min(48, sfs - 8));
    for (int i = 1; i <= 5; ++i) {
        drawTextLeft(cr, slotSummary(i), left, y + std::max(38, bh - 12), fs, false);
        RectButton save{"Save " + std::to_string(i), left + summaryW, y, smallW, bh};
        RectButton load{"Load " + std::to_string(i), left + summaryW + smallW + 8, y, smallW, bh};
        RectButton del{"Del " + std::to_string(i), left + summaryW + (smallW + 8) * 2, y, smallW, bh};
        app.overlayButtons.push_back(save); app.overlayButtons.push_back(load); app.overlayButtons.push_back(del);
        drawButton(cr, save, fs); drawButton(cr, load, fs); drawButton(cr, del, fs);
        y += rowH;
    }
    RectButton back{"Back", left, (int)(oy + oh - 78), 190, 62};
    RectButton close{"Close Settings", left + 210, (int)(oy + oh - 78), 320, 62};
    app.overlayButtons.push_back(back); app.overlayButtons.push_back(close);
    drawButton(cr, back, fs);
    drawButton(cr, close, fs);
}

static void drawSettings(cairo_t* cr, const Layout& L) {
    app.overlayButtons.clear();
    double ow = std::min((double)L.W * 0.96, 1040.0);
    double oh = std::min((double)L.H * 0.94, 1180.0);
    double ox = (L.W - ow) / 2.0;
    double oy = (L.H - oh) / 2.0;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.90);
    cairo_rectangle(cr, ox, oy, ow, oh);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0,0,0);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);

    if (app.settingsPage == 1) drawSaveLoadSettings(cr, L, ox, oy, ow, oh);
    else drawSettingsMain(cr, L, ox, oy, ow, oh);
}

static void drawMoveConfirm(cairo_t* cr, const Layout& L) {
    app.overlayButtons.clear();
    double ow = std::min((double)L.W * 0.84, 620.0);
    double oh = 230;
    double ox = (L.W - ow) / 2.0;
    double oy = (L.H - oh) / 2.0;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.90);
    cairo_rectangle(cr, ox, oy, ow, oh);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0,0,0);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);
    std::string text = "Confirm move " + square_name(app.confirmFrom) + "-" + square_name(app.confirmTo);
    if (app.confirmPromotion) {
        text += "=";
        text.push_back((char)std::toupper((unsigned char)app.confirmPromotion));
    }
    text += "?";
    drawTextCentered(cr, text, ox + 20, oy + 24, ow - 40, 60, std::min(34, app.uiFontSize + 3), true);
    int bw = 190, bh = 56;
    RectButton cancel{"Cancel", (int)(ox + ow/2 - bw - 12), (int)(oy + oh - bh - 28), bw, bh};
    RectButton yes{"Confirm", (int)(ox + ow/2 + 12), (int)(oy + oh - bh - 28), bw, bh};
    app.overlayButtons.push_back(cancel); app.overlayButtons.push_back(yes);
    drawButton(cr, cancel, std::max(16, std::min(24, app.uiFontSize - 2)));
    drawButton(cr, yes, std::max(16, std::min(24, app.uiFontSize - 2)));
}

static void drawReviewControls(cairo_t* cr, const Layout& L) {
    if (!app.reviewMode) { app.reviewButtons.clear(); return; }
    app.reviewButtons.clear();
    int total = (int)app.game.uciMoves().size();
    int w = std::min(L.W - 40, 680);
    int h = 92;
    int x = (L.W - w) / 2;
    int y = L.H - L.statusH - h - 8;
    if (y < L.boardY + L.board - h) y = L.boardY + L.board - h - 10;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.90);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0,0,0);
    cairo_set_line_width(cr, 3.0);
    cairo_stroke(cr);
    drawTextCentered(cr, "Review: move " + std::to_string(app.reviewPly) + " / " + std::to_string(total), x, y + 4, w, 30, std::max(13, std::min(22, app.uiFontSize - 5)), true);
    int bw = (w - 48) / 3;
    RectButton prev{"Prev", x + 12, y + 42, bw, 40};
    RectButton next{"Next", x + 24 + bw, y + 42, bw, 40};
    RectButton exit{"Exit Review", x + 36 + bw * 2, y + 42, bw, 40};
    app.reviewButtons.push_back(prev); app.reviewButtons.push_back(next); app.reviewButtons.push_back(exit);
    drawButton(cr, prev, std::max(13, std::min(20, app.uiFontSize - 6)));
    drawButton(cr, next, std::max(13, std::min(20, app.uiFontSize - 6)));
    drawButton(cr, exit, std::max(13, std::min(20, app.uiFontSize - 6)));
}

static void drawGameOver(cairo_t* cr, const Layout& L) {
    app.overlayButtons.clear();
    double ow = std::min((double)L.W * 0.84, 650.0);
    double oh = 250;
    double ox = (L.W - ow) / 2.0;
    double oy = (L.H - oh) / 2.0;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.90);
    cairo_rectangle(cr, ox, oy, ow, oh);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0,0,0);
    cairo_set_line_width(cr, 4.0);
    cairo_stroke(cr);
    drawTextCentered(cr, app.gameOverText.empty() ? "Game Over" : app.gameOverText, ox + 20, oy + 28, ow - 40, 76, app.uiFontSize + 5, true);
    int bw = std::max(230, std::min(320, (int)(ow - 80)));
    int bh = std::max(62, std::min(78, app.uiFontSize + 24));
    RectButton yes{"New Game", (int)(ox + ow/2 - bw/2), (int)(oy + oh - bh - 24), bw, bh};
    app.overlayButtons.push_back(yes);
    drawButton(cr, yes, std::max(17, app.uiFontSize - 1));
}

static gboolean on_draw(GtkWidget* widget, GdkEventExpose*, gpointer) {
    cairo_t* cr = gdk_cairo_create(widget->window);
    Layout L = computeLayout(widget);
    int W = L.W, H = L.H;

    cairo_set_source_rgb(cr, 0.92, 0.92, 0.90);
    cairo_paint(cr);

    drawToolbar(cr, W);

    Position displayPos = app.reviewMode ? app.game.positionAfterPly(app.reviewPly) : app.game.state();

    std::vector<Move> selectedMoves;
    if (!app.reviewMode && app.selected >= 0) selectedMoves = app.game.legalMovesFrom(app.selected);

    int lastFrom = -1, lastTo = -1;
    if (app.reviewMode) {
        const auto& moves = app.game.uciMoves();
        if (app.reviewPly > 0 && app.reviewPly <= (int)moves.size()) {
            const std::string& u = moves[app.reviewPly - 1];
            if (u.size() >= 4) { lastFrom = parse_square(u.substr(0,2)); lastTo = parse_square(u.substr(2,2)); }
        }
    } else {
        lastMoveSquares(lastFrom, lastTo);
    }

    for (int vr = 0; vr < 8; ++vr) {
        for (int vf = 0; vf < 8; ++vf) {
            int br = app.flipped ? 7 - vr : vr;
            int bf = app.flipped ? 7 - vf : vf;
            int s = sq(br, bf);
            bool light = ((br + bf) % 2 == 0);
            if (light) cairo_set_source_rgb(cr, 0.82, 0.82, 0.78);
            else cairo_set_source_rgb(cr, 0.55, 0.55, 0.52);
            cairo_rectangle(cr, L.boardX + vf * L.cell, L.boardY + vr * L.cell, L.cell, L.cell);
            cairo_fill(cr);

            if (s == lastFrom || s == lastTo) {
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.18);
                cairo_rectangle(cr, L.boardX + vf * L.cell + 4, L.boardY + vr * L.cell + 4, L.cell - 8, L.cell - 8);
                cairo_fill(cr);
            }

            if (!app.reviewMode && (s == app.hintFrom || s == app.hintTo)) {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.42);
                cairo_rectangle(cr, L.boardX + vf * L.cell + 8, L.boardY + vr * L.cell + 8, L.cell - 16, L.cell - 16);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                cairo_set_line_width(cr, 4.0);
                cairo_rectangle(cr, L.boardX + vf * L.cell + 8, L.boardY + vr * L.cell + 8, L.cell - 16, L.cell - 16);
                cairo_stroke(cr);
                cairo_set_line_width(cr, 1.0);
            }

            if (!app.reviewMode && s == app.selected) {
                cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
                cairo_set_line_width(cr, 4.0);
                cairo_rectangle(cr, L.boardX + vf * L.cell + 3, L.boardY + vr * L.cell + 3, L.cell - 6, L.cell - 6);
                cairo_stroke(cr);
                cairo_set_line_width(cr, 1.0);
            }

            bool isTarget = false;
            for (const auto& m : selectedMoves) if (m.to == s) { isTarget = true; break; }
            if (isTarget) {
                cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.58);
                if (displayPos.b[s] == '.') {
                    cairo_arc(cr, L.boardX + vf * L.cell + L.cell / 2.0, L.boardY + vr * L.cell + L.cell / 2.0, L.cell * 0.12, 0, 6.28318);
                    cairo_fill(cr);
                } else {
                    cairo_set_line_width(cr, 5.0);
                    cairo_rectangle(cr, L.boardX + vf * L.cell + 5, L.boardY + vr * L.cell + 5, L.cell - 10, L.cell - 10);
                    cairo_stroke(cr);
                    cairo_set_line_width(cr, 1.0);
                }
            }

            char p = displayPos.b[s];
            if (p != '.') {
                if (!drawPieceImage(cr, p, L.boardX + vf * L.cell, L.boardY + vr * L.cell, L.cell)) {
                    double cx = L.boardX + vf * L.cell + L.cell / 2.0;
                    double cy = L.boardY + vr * L.cell + L.cell / 2.0;
                    double rad = L.cell * 0.36;
                    bool drawAsWhite = is_white_piece(p);
                    if (shouldInvertPieceImages()) drawAsWhite = !drawAsWhite;
                    if (drawAsWhite) {
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
                    drawTextCentered(cr, pieceLetter(p), L.boardX + vf * L.cell, L.boardY + vr * L.cell + 2, L.cell, L.cell, L.cell * 0.46, true);
                }
            }
        }
    }

    drawCoordinates(cr, L);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, L.boardX, L.boardY, L.board, L.board);
    cairo_stroke(cr);

    drawMovePanel(cr, L);
    drawCapturedPieces(cr, L);

    std::string status = app.reviewMode ? ("Review mode. " + std::to_string(app.reviewPly) + " / " + std::to_string((int)app.game.uciMoves().size()) + " plies") : (app.uiStatus.empty() ? app.game.status() : app.uiStatus);
    if (!app.reviewMode && app.uiStatus.empty() && !hintMoveText().empty()) status = hintMoveText();
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    drawTextCentered(cr, status, L.margin, H - L.statusH + 6, W - 2 * L.margin, L.statusH * 0.58, app.uiFontSize + 2, true);
    drawTextCentered(cr, moveSummaryLine(), L.margin, H - L.statusH + L.statusH * 0.58, W - 2 * L.margin, L.statusH * 0.36, std::max(12, app.uiFontSize - 7), false);

    if (app.pendingFrom >= 0) {
        double ox = L.boardX + L.board * 0.1;
        double oy = L.boardY + L.board * 0.36;
        double ow = L.board * 0.8;
        double oh = std::max(190.0, L.board * 0.28);
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.90);
        cairo_rectangle(cr, ox, oy, ow, oh);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_set_line_width(cr, 3.0);
        cairo_stroke(cr);
        drawTextCentered(cr, "Promote to", ox, oy + 8, ow, 38, app.uiFontSize, true);
        const char* opts[4] = {"Q", "R", "B", "N"};
        for (int i = 0; i < 4; ++i) {
            double x = ox + 18 + i * (ow - 36) / 4.0;
            double y = oy + 58;
            double w = (ow - 52) / 4.0;
            double h = oh - 76;
            cairo_rectangle(cr, x, y, w, h);
            cairo_stroke(cr);
            drawTextCentered(cr, opts[i], x, y, w, h, std::max(30, app.uiFontSize + 8), true);
        }
    }

    drawReviewControls(cr, L);

    if (app.showGameOver) drawGameOver(cr, L);
    else if (app.confirmNew) drawConfirmNew(cr, L);
    else if (app.showMoveConfirm) drawMoveConfirm(cr, L);
    else if (app.showSettings) drawSettings(cr, L);

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
    int elo = currentEngineElo();
    int mt = engineMovetimeForElo(elo);
    app.uiStatus = "Engine thinking at Elo " + std::to_string(elo) + "...";
    flush_gui();
    std::string err;
    std::string mv = app.engine.bestMove(engine_path(), app.game.uciMoves(), mt, elo, err);
    app.uiStatus.clear();
    if (mv.empty()) {
        app.uiStatus = err.empty() ? "Engine produced no move." : err;
    } else if (!app.game.makeUciMove(mv)) {
        app.uiStatus = "Engine sent illegal move: " + mv;
    } else {
        app.game.save();
        updateGameOverPopup();
    }
    if (app.area) gtk_widget_queue_draw(app.area);
}

static int boardSquareFromXY(GtkWidget* widget, int x, int y) {
    Layout L = computeLayout(widget);
    if (x < L.boardX || x >= L.boardX + L.board || y < L.boardY || y >= L.boardY + L.board) return -1;
    int vf = (x - L.boardX) / L.cell;
    int vr = (y - L.boardY) / L.cell;
    int br = app.flipped ? 7 - vr : vr;
    int bf = app.flipped ? 7 - vf : vf;
    return sq(br, bf);
}

static void clearPendingMoveConfirm() {
    app.showMoveConfirm = false;
    app.confirmFrom = app.confirmTo = -1;
    app.confirmPromotion = 0;
}

static void beginMoveConfirm(int from, int to, char promo = 0) {
    app.confirmFrom = from;
    app.confirmTo = to;
    app.confirmPromotion = promo;
    app.showMoveConfirm = true;
    app.selected = -1;
    app.pendingFrom = app.pendingTo = -1;
}

static void commitHumanMove(int from, int to, char promo = 0) {
    app.resigned = false;
    app.resignedMessage.clear();
    app.showGameOver = false;
    app.gameOverText.clear();
    app.hintFrom = app.hintTo = -1;
    clearPendingMoveConfirm();
    if (app.game.makeMove(from, to, promo)) {
        app.game.save();
        app.selected = -1;
        gtk_widget_queue_draw(app.area);
        if (!updateGameOverPopup()) maybeEngineMove();
    }
}

static void requestHint() {
    if (app.reviewMode) { app.uiStatus = "Exit review mode before requesting a hint."; return; }
    if (app.game.legalMoves().empty()) { app.uiStatus = "No legal moves available."; return; }
    int elo = currentEngineElo();
    app.uiStatus = "Stockfish calculating hint...";
    flush_gui();
    std::string err;
    std::string mv = app.engine.bestMove(engine_path(), app.game.uciMoves(), std::max(750, engineMovetimeForElo(elo)), elo, err);
    if (mv.size() >= 4) {
        app.hintFrom = parse_square(mv.substr(0,2));
        app.hintTo = parse_square(mv.substr(2,2));
        app.uiStatus = "Hint: " + mv.substr(0,2) + "-" + mv.substr(2,2);
    } else {
        app.hintFrom = app.hintTo = -1;
        app.uiStatus = err.empty() ? "No hint available." : err;
    }
}

static bool handlePromotionTap(GtkWidget* widget, int x, int y) {
    if (app.pendingFrom < 0) return false;
    Layout L = computeLayout(widget);
    double ox = L.boardX + L.board * 0.1;
    double oy = L.boardY + L.board * 0.36;
    double ow = L.board * 0.8;
    double oh = std::max(190.0, L.board * 0.28);
    const char promos[4] = {'q','r','b','n'};
    for (int i = 0; i < 4; ++i) {
        double bx = ox + 18 + i * (ow - 36) / 4.0;
        double by = oy + 58;
        double bw = (ow - 52) / 4.0;
        double bh = oh - 76;
        if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
            if (app.confirmMoves) beginMoveConfirm(app.pendingFrom, app.pendingTo, promos[i]);
            else commitHumanMove(app.pendingFrom, app.pendingTo, promos[i]);
            gtk_widget_queue_draw(app.area);
            return true;
        }
    }
    return true;
}

static bool pointInButton(const RectButton& b, int x, int y) {
    return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

static void startNewGameNow() {
    app.uiStatus.clear();
    app.resigned = false;
    app.resignedMessage.clear();
    app.showGameOver = false;
    app.gameOverText.clear();
    app.confirmNew = false;
    app.showSettings = false;
    app.settingsPage = 0;
    app.reviewMode = false;
    app.reviewPly = 0;
    app.hintFrom = app.hintTo = -1;
    clearPendingMoveConfirm();
    app.game.reset();
    app.selected = -1;
    app.pendingFrom = app.pendingTo = -1;
    app.game.save();
    maybeEngineMove();
}

static bool handleConfirmTap(int x, int y) {
    if (!app.confirmNew) return false;
    for (const auto& b : app.overlayButtons) {
        if (!pointInButton(b, x, y)) continue;
        if (b.label == "Cancel") app.confirmNew = false;
        else if (b.label == "New Game") startNewGameNow();
        gtk_widget_queue_draw(app.area);
        return true;
    }
    return true;
}

static bool handleMoveConfirmTap(int x, int y) {
    if (!app.showMoveConfirm) return false;
    for (const auto& b : app.overlayButtons) {
        if (!pointInButton(b, x, y)) continue;
        if (b.label == "Cancel") clearPendingMoveConfirm();
        else if (b.label == "Confirm") commitHumanMove(app.confirmFrom, app.confirmTo, app.confirmPromotion);
        gtk_widget_queue_draw(app.area);
        return true;
    }
    return true;
}

static bool handleReviewTap(int x, int y) {
    if (!app.reviewMode) return false;
    for (const auto& b : app.reviewButtons) {
        if (!pointInButton(b, x, y)) continue;
        int total = (int)app.game.uciMoves().size();
        if (b.label == "Prev") app.reviewPly = clampInt(app.reviewPly - 1, 0, total);
        else if (b.label == "Next") app.reviewPly = clampInt(app.reviewPly + 1, 0, total);
        else if (b.label == "Exit Review") { app.reviewMode = false; app.reviewPly = 0; }
        gtk_widget_queue_draw(app.area);
        return true;
    }
    return false;
}

static bool handleGameOverTap(int x, int y) {
    if (!app.showGameOver) return false;
    for (const auto& b : app.overlayButtons) {
        if (!pointInButton(b, x, y)) continue;
        if (b.label == "New Game") startNewGameNow();
        gtk_widget_queue_draw(app.area);
        return true;
    }
    return true;
}

static bool handleSettingsTap(int x, int y) {
    if (!app.showSettings) return false;
    for (const auto& b : app.overlayButtons) {
        if (!pointInButton(b, x, y)) continue;
        bool locked = gameSetupLocked();

        if (app.settingsPage == 1) {
            if (b.label == "Back") app.settingsPage = 0;
            else if (b.label == "Close Settings") { app.showSettings = false; app.settingsPage = 0; }
            else if (b.label.rfind("Save ", 0) == 0) {
                int n = std::atoi(b.label.substr(5).c_str());
                if (n >= 1 && n <= 5) saveSlot(n);
            } else if (b.label.rfind("Load ", 0) == 0) {
                int n = std::atoi(b.label.substr(5).c_str());
                if (n >= 1 && n <= 5) loadSlot(n);
            } else if (b.label.rfind("Del ", 0) == 0) {
                int n = std::atoi(b.label.substr(4).c_str());
                if (n >= 1 && n <= 5) deleteSlot(n);
            }
            saveAppSettings();
            gtk_widget_queue_draw(app.area);
            return true;
        }

        if (b.label.rfind("Coordinates", 0) == 0) app.showCoordinates = !app.showCoordinates;
        else if (b.label.rfind("Move List", 0) == 0) app.showMoveList = !app.showMoveList;
        else if (b.label == "Font -") app.uiFontSize = clampInt(app.uiFontSize - 2, 14, 50);
        else if (b.label == "Font +") app.uiFontSize = clampInt(app.uiFontSize + 2, 14, 50);
        else if (b.label.rfind("Engine", 0) == 0 && !locked) {
            if (app.engineMode == EngineMode::Off) app.engineMode = EngineMode::Black;
            else if (app.engineMode == EngineMode::Black) app.engineMode = EngineMode::White;
            else app.engineMode = EngineMode::Off;
        } else if (b.label == "Elo -250" && !locked) {
            app.engineEloIndex = clampInt(app.engineEloIndex - 1, 0, (int)app.engineElos.size() - 1);
        } else if (b.label == "Elo +250" && !locked) {
            app.engineEloIndex = clampInt(app.engineEloIndex + 1, 0, (int)app.engineElos.size() - 1);
        } else if (b.label == "Hint") {
            app.showSettings = false;
            requestHint();
        } else if (b.label == "Restart Engine") {
            app.engine.stop();
            app.uiStatus = "Engine restarted.";
        } else if (b.label == "Export PGN") {
            app.uiStatus = exportPGN();
        } else if (b.label.rfind("Confirm Moves", 0) == 0) {
            app.confirmMoves = !app.confirmMoves;
        } else if (b.label == "Save / Load") {
            app.settingsPage = 1;
        } else if (b.label == "Review Game") {
            app.reviewMode = true;
            app.reviewPly = (int)app.game.uciMoves().size();
            app.showSettings = false;
            app.selected = -1;
            app.hintFrom = app.hintTo = -1;
            app.uiStatus = "Review mode.";
        } else if (b.label.rfind("Piece PNGs", 0) == 0) app.usePieceImages = !app.usePieceImages;
        else if (b.label.rfind("Dark Piece Fix", 0) == 0) {
            app.darkPieceMode = (app.darkPieceMode + 1) % 3;
            app.uiStatus = std::string("Dark Piece Fix: ") + darkPieceModeLabel();
        }
        else if (b.label == "Reload PNGs") {
            freePieceImages();
            loadPieceImages();
            app.uiStatus = haveAllPieceImages() ? "Piece PNGs loaded." : "Some piece PNGs are missing; using fallback letters.";
        } else if (b.label == "Close Settings") { app.showSettings = false; app.settingsPage = 0; }
        saveAppSettings();
        gtk_widget_queue_draw(app.area);
        return true;
    }
    return true;
}

static void clickButton(const std::string& label) {
    app.uiStatus.clear();
    if (label == "New") {
        app.confirmNew = true;
        app.settingsPage = 0;
        app.showSettings = false;
        app.showGameOver = false;
        app.overlayButtons.clear();
    } else if (label == "Undo") {
        if (app.reviewMode) { app.uiStatus = "Exit review mode before undo."; gtk_widget_queue_draw(app.area); return; }
        app.resigned = false;
        app.resignedMessage.clear();
        app.showGameOver = false;
        app.gameOverText.clear();
        app.hintFrom = app.hintTo = -1;
        app.game.undo(); app.game.save(); app.selected = -1;
        if (engineShouldMove()) { app.game.undo(); app.game.save(); }
    } else if (label == "Settings") {
        app.showSettings = true;
        app.settingsPage = 0;
        app.confirmNew = false;
        app.showGameOver = false;
        app.overlayButtons.clear();
    } else if (label == "Exit") {
        app.game.save();
        saveAppSettings();
        app.engine.stop();
        freePieceImages();
        gtk_main_quit();
        return;
    }
    gtk_widget_queue_draw(app.area);
}

static gboolean on_button(GtkWidget* widget, GdkEventButton* ev, gpointer) {
    if (ev->button != 1) return FALSE;
    int x = (int)ev->x, y = (int)ev->y;

    if (handleGameOverTap(x, y)) return TRUE;
    if (handleConfirmTap(x, y)) return TRUE;
    if (handleMoveConfirmTap(x, y)) return TRUE;
    if (handleSettingsTap(x, y)) return TRUE;
    if (handlePromotionTap(widget, x, y)) return TRUE;
    if (handleReviewTap(x, y)) return TRUE;

    for (const auto& b : app.buttons) {
        if (pointInButton(b, x, y)) {
            clickButton(b.label);
            return TRUE;
        }
    }
    if (app.reviewMode) return TRUE;
    if (app.resigned) return TRUE;
    if (app.showGameOver) return TRUE;
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
            } else {
                bool legalDest = false;
                for (const auto& m : app.game.legalMovesFrom(app.selected)) if (m.to == s && !m.promotion) { legalDest = true; break; }
                if (!legalDest) app.uiStatus = "Illegal move.";
                else if (app.confirmMoves) beginMoveConfirm(app.selected, s, 0);
                else commitHumanMove(app.selected, s, 0);
            }
        }
    }
    gtk_widget_queue_draw(app.area);
    return TRUE;
}

static gboolean on_delete(GtkWidget*, GdkEvent*, gpointer) {
    app.game.save();
    saveAppSettings();
    app.engine.stop();
    freePieceImages();
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
    loadAppSettings();
    app.flipped = false;
    app.game.load();
    updateGameOverPopup();

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
