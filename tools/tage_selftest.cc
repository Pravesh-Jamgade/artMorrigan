// ─────────────────────────────────────────────────────────────────────────────
// tage_selftest.cc  —  standalone proof of the T-IRIP train/lookup key bug
//
// Reproduces ONLY the T-IRIP indexing logic from morriganPT_tage.stlb_pref,
// with no ChampSim dependency, and drives it with a synthetic iSTLB miss
// stream that has strong, learnable history correlation.
//
// A correct history predictor should reach a very high hit rate on this
// stream. The BUGGY key scheme reaches ~0%, which is what we measured on the
// real 45-trace sweep (0.27%).
//
// Build:  g++ -O2 -std=c++11 -o tage_selftest tage_selftest.cc
// Run:    ./tage_selftest
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

#define TAG_BITS 16
#define TAG_MASK ((1 << TAG_BITS) - 1)

#define THT_H2_SETS  128
#define THT_H2_ASSOC 4
#define THT_H4_SETS  128
#define THT_H4_ASSOC 4
#define THT_H8_SETS  64
#define THT_H8_ASSOC 4
#define TAGE_MAX_HIST 8
#define TAGE_NTAB 3

static const int TAGE_HLEN[TAGE_NTAB] = {2, 4, 8};

uint32_t hash32(uint32_t a) {
    a = (a + 0x479ab41d) + (a << 8);
    a = (a ^ 0xe4aa10ce) ^ (a >> 5);
    a = (a + 0x9942f0a6) - (a << 14);
    a = (a ^ 0x5aedd67d) ^ (a >> 3);
    a = (a + 0x17bea992) + (a << 7);
    return a;
}

struct Entry {
    uint64_t tag = 0;
    int64_t  delta = 0;
    int      confidence = 0;
    uint64_t timestamp = 0;
    bool     valid = false;
};

struct Tage {
    std::vector<std::vector<Entry> > tab[TAGE_NTAB];
    uint64_t history_reg[TAGE_MAX_HIST];
    int      history_len;
    uint64_t timer;

    // stats
    uint64_t lookups, hits, correct;

    // pending keys (used only by the FIXED scheme)
    int      pend_idx[TAGE_NTAB];
    uint64_t pend_tag[TAGE_NTAB];
    int      pend_ok[TAGE_NTAB];
    int      pend_valid;
    int64_t  last_pred_delta;

    int sets(int t)  const { return t == 0 ? THT_H2_SETS  : t == 1 ? THT_H4_SETS  : THT_H8_SETS;  }
    int assoc(int t) const { return t == 0 ? THT_H2_ASSOC : t == 1 ? THT_H4_ASSOC : THT_H8_ASSOC; }

    Tage() {
        for (int t = 0; t < TAGE_NTAB; t++) {
            int ns = (int)pow(2.0, ceil(log2((double)sets(t) / assoc(t))));
            tab[t].assign(ns, std::vector<Entry>(assoc(t)));
        }
        memset(history_reg, 0, sizeof(history_reg));
        history_len = 0; timer = 0;
        lookups = hits = correct = 0;
        for (int t = 0; t < TAGE_NTAB; t++) { pend_idx[t] = -1; pend_tag[t] = 0; pend_ok[t] = 0; }
        pend_valid = 0; last_pred_delta = 0;
    }

    uint64_t fold_history(int len) const {
        uint64_t h = 0;
        int n = (len < history_len) ? len : history_len;
        for (int i = 0; i < n; i++) {
            uint64_t v = history_reg[i] & TAG_MASK;
            int rot = i & 7;
            v = (rot == 0) ? v : (((v << rot) | (v >> (16 - rot))) & TAG_MASK);
            h ^= v;
        }
        return h;
    }

    int idx_of(uint64_t pv, int hlen, int t) const {
        uint64_t h = fold_history(hlen);
        uint32_t raw = hash32((uint32_t)(pv ^ h));
        int ns = sets(t) / assoc(t);
        int bits = (int)ceil(log2((double)ns));
        return (int)(raw & ((1 << bits) - 1));
    }
    uint64_t tag_of(uint64_t pv, int hlen) const {
        uint64_t h = fold_history(hlen);
        return hash32((uint32_t)(pv ^ (h >> 3))) & TAG_MASK;
    }

    void keys(uint64_t pv, int* idx, uint64_t* tg, int* ok, bool gate_per_table) const {
        for (int t = 0; t < TAGE_NTAB; t++) {
            int hlen = TAGE_HLEN[t];
            // BUGGY gate: one global "history_len >= 2" test for all tables.
            // FIXED gate: each table needs its own history length.
            int eligible = gate_per_table ? (history_len >= hlen) : (history_len >= 2);
            if (!eligible) { ok[t] = 0; idx[t] = -1; tg[t] = 0; continue; }
            ok[t] = 1;
            idx[t] = idx_of(pv, hlen, t);
            tg[t] = tag_of(pv, hlen);
        }
    }

    int search(int t, int idx, uint64_t tg, bool use_valid_bit) {
        std::vector<Entry>& row = tab[t][idx];
        for (int i = 0; i < (int)row.size(); i++) {
            bool live = use_valid_bit ? row[i].valid : (row[i].delta != 0);
            if (live && row[i].tag == tg) return i;
        }
        return -1;
    }

    int victim(int t, int idx) {
        std::vector<Entry>& row = tab[t][idx];
        for (int i = 0; i < (int)row.size(); i++) if (!row[i].valid) return i;
        int v = 0;
        for (int i = 1; i < (int)row.size(); i++)
            if (row[i].timestamp < row[v].timestamp) v = i;
        return v;
    }

    int64_t lookup(const int* idx, const uint64_t* tg, const int* ok, bool use_valid_bit) {
        for (int t = TAGE_NTAB - 1; t >= 0; t--) {
            if (!ok[t]) continue;
            int w = search(t, idx[t], tg[t], use_valid_bit);
            if (w >= 0 && tab[t][idx[t]][w].confidence >= 1 && tab[t][idx[t]][w].delta != 0) {
                tab[t][idx[t]][w].timestamp = timer;
                return tab[t][idx[t]][w].delta;
            }
        }
        return 0;
    }

    void update(const int* idx, const uint64_t* tg, const int* ok,
                int64_t delta, bool use_valid_bit) {
        if (delta == 0) return;
        for (int t = 0; t < TAGE_NTAB; t++) {
            if (!ok[t]) continue;
            int w = search(t, idx[t], tg[t], use_valid_bit);
            if (w >= 0) {
                Entry& e = tab[t][idx[t]][w];
                if (e.delta == delta) { if (e.confidence < 3) e.confidence++; }
                else { if (e.confidence > 0) e.confidence--;
                       if (e.confidence == 0) { e.delta = delta; e.confidence = 1; } }
                e.timestamp = timer;
            } else {
                int v = victim(t, idx[t]);
                Entry& e = tab[t][idx[t]][v];
                e.tag = tg[t]; e.delta = delta; e.confidence = 1;
                e.timestamp = timer; e.valid = true;
            }
        }
    }

    void push_history(uint64_t pv) {
        for (int i = TAGE_MAX_HIST - 1; i > 0; i--) history_reg[i] = history_reg[i - 1];
        history_reg[0] = pv;
        if (history_len < TAGE_MAX_HIST) history_len++;
    }
};

// ── BUGGY flow: recompute the training key at training time ─────────────────
void run_buggy(Tage& g, const std::vector<uint64_t>& stream) {
    uint64_t prev = 0; bool have_prev = false;
    for (size_t n = 0; n < stream.size(); n++) {
        uint64_t vpn = stream[n];
        uint64_t pv = vpn & TAG_MASK;
        g.timer++;

        int idx[TAGE_NTAB]; uint64_t tg[TAGE_NTAB]; int ok[TAGE_NTAB];
        g.keys(pv, idx, tg, ok, /*gate_per_table=*/false);

        int64_t pred = 0;
        if (g.history_len >= 2) {
            g.lookups++;
            pred = g.lookup(idx, tg, ok, /*use_valid_bit=*/false);
            if (pred != 0) g.hits++;
        }
        if (pred != 0 && have_prev && pred == (int64_t)vpn - (int64_t)prev) {
            // scored against the wrong edge on purpose? no: score vs NEXT edge
        }
        int64_t actual = have_prev ? (int64_t)vpn - (int64_t)prev : 0;
        if (g.last_pred_delta != 0 && g.last_pred_delta == actual) g.correct++;
        g.last_pred_delta = pred;

        if (have_prev) {
            // *** THE BUG *** key recomputed here, with prev already sitting in
            // history_reg[0] from the end of the previous iteration.
            int bidx[TAGE_NTAB]; uint64_t btg[TAGE_NTAB]; int bok[TAGE_NTAB];
            g.keys(prev & TAG_MASK, bidx, btg, bok, /*gate_per_table=*/false);
            g.update(bidx, btg, bok, actual, /*use_valid_bit=*/false);
        }
        g.push_history(pv);
        prev = vpn; have_prev = true;
    }
}

// ── FIXED flow: snapshot the lookup key, replay it at training time ─────────
void run_fixed(Tage& g, const std::vector<uint64_t>& stream) {
    uint64_t prev = 0; bool have_prev = false;
    for (size_t n = 0; n < stream.size(); n++) {
        uint64_t vpn = stream[n];
        uint64_t pv = vpn & TAG_MASK;
        g.timer++;

        int idx[TAGE_NTAB]; uint64_t tg[TAGE_NTAB]; int ok[TAGE_NTAB];
        g.keys(pv, idx, tg, ok, /*gate_per_table=*/true);

        g.lookups++;
        int64_t pred = g.lookup(idx, tg, ok, /*use_valid_bit=*/true);
        if (pred != 0) g.hits++;

        int64_t actual = have_prev ? (int64_t)vpn - (int64_t)prev : 0;
        if (g.last_pred_delta != 0 && g.last_pred_delta == actual) g.correct++;
        g.last_pred_delta = pred;

        if (have_prev && g.pend_valid)
            g.update(g.pend_idx, g.pend_tag, g.pend_ok, actual, /*use_valid_bit=*/true);

        // carry keys forward BEFORE the history shifts
        for (int t = 0; t < TAGE_NTAB; t++) {
            g.pend_idx[t] = idx[t]; g.pend_tag[t] = tg[t]; g.pend_ok[t] = ok[t];
        }
        g.pend_valid = 1;

        g.push_history(pv);
        prev = vpn; have_prev = true;
    }
}

int main() {
    // Synthetic miss stream with genuine history correlation: a handful of
    // repeating page-walk "routes" of length 12, visited in a rotating order.
    // Every successor is perfectly determined by the preceding context, so an
    // exact-match history predictor should approach 100% hit rate.
    std::vector<uint64_t> stream;
    const uint64_t base[4] = {0x10000, 0x20000, 0x30000, 0x40000};
    const int64_t  route[4][12] = {
        { 3,  1,  4,  1,  5,  9,  2,  6,  5,  3,  5, -34},
        { 7, -2,  9,  1, -4,  8,  3,  3, -1,  6,  2, -32},
        { 1,  1,  2,  3,  5,  8, 13, -7, -5, -4, -3, -14},
        {-1,  4, -2,  7,  1,  1,  9, -3,  6,  2, -8,  -16},
    };
    for (int rep = 0; rep < 3000; rep++) {
        int r = rep & 3;
        uint64_t p = base[r];
        for (int k = 0; k < 12; k++) { stream.push_back(p); p += route[r][k]; }
    }

    printf("Synthetic iSTLB miss stream: %zu misses, 4 deterministic routes\n", stream.size());
    printf("(every successor is fully determined by its history context)\n\n");

    Tage a; run_buggy(a, stream);
    Tage b; run_fixed(b, stream);

    printf("%-34s %12s %12s %12s\n", "", "lookups", "table hits", "hit rate");
    printf("%-34s %12llu %12llu %11.3f%%\n", "BUGGY (key recomputed at train)",
           (unsigned long long)a.lookups, (unsigned long long)a.hits,
           a.lookups ? 100.0 * a.hits / a.lookups : 0.0);
    printf("%-34s %12llu %12llu %11.3f%%\n", "FIXED (key snapshotted)",
           (unsigned long long)b.lookups, (unsigned long long)b.hits,
           b.lookups ? 100.0 * b.hits / b.lookups : 0.0);

    printf("\n%-34s %12s %12s\n", "", "correct", "accuracy");
    printf("%-34s %12llu %11.3f%%\n", "BUGGY",
           (unsigned long long)a.correct, a.hits ? 100.0 * a.correct / a.hits : 0.0);
    printf("%-34s %12llu %11.3f%%\n", "FIXED",
           (unsigned long long)b.correct, b.hits ? 100.0 * b.correct / b.hits : 0.0);

    printf("\nExpected: BUGGY near 0%% hit rate, FIXED near 100%%.\n");
    return 0;
}
