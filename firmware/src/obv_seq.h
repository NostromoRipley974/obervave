/* SPDX-License-Identifier: GPL-3.0-only */
/* The step sequencer: four tracks A-D, each with its own 256 steps, length, note value, swing and
 * transpose, each playing its own sound. A step holds a chord of up to four notes, a velocity, a gate (or a tie into
 * the next step), a ratchet (1-4 hits), a chance and a nudge (how late in the step it sounds, for unquantised
 * recording). The tracks start and stop on the bar (as clips in Ableton Live): a request waits for the next downbeat;
 * the next pattern comes in on a downbeat too.
 * The player runs in the audio ISR per control block, as the arpeggiator does; the main loop edits `pat`. */
#define NTRK 4u
#define SEQ_STEPS 256u
#define SEQ_CHORD 4u
#define GATE_TIE 127u                           /* a gate of 127: the notes hold into the next step */
#define NPAT NBLOB                              /* patterns, one flash slot each (4 sectors) */
#ifndef TRACE
#define TRACE(k, a, b) ((void)0)
#endif
#define PAT_MAGIC 0x34514553u                   /* "SEQ4" */
#define BAR_BEATS 4u

typedef struct { uint8_t note[SEQ_CHORD], vel, gate, ratch, chance, nudge; } step_t;   /* note 0 = none; ratch 0..3 = 1..4 hits;
                                                                                         nudge 0..127: that much of the step later */
#define STEP_OFF 0x80u                          /* in ratch: the step is switched off, its notes kept */
typedef struct { uint16_t len; uint8_t div, swing; int8_t transp; uint8_t mute, pad[2]; } seqtrk_t;   /* swing 50..75 % */
/* (OberVave 3) the knobs recorded: per track up to NLANE lanes, each one parameter of the track's sound with a value per
 * step (AUTO_NONE: that step leaves it as it is). REC running + a knob turned = the value written on the step playing;
 * the player sets it on its step. After the steps, behind auto_magic: a pattern saved before has none (cleared at load) */
#define NLANE 4u
#define AUTO_NONE 0xFFu
#define AUTO_MAGIC 0x4F545541u                  /* "AUTO" */
typedef struct { uint8_t param, pad[3]; uint8_t v[SEQ_STEPS]; } lane_t;
typedef struct { uint32_t magic; seqtrk_t trk[NTRK]; step_t st[NTRK][SEQ_STEPS]; uint32_t auto_magic; lane_t lane[NTRK][NLANE]; } pattern_t;

static pattern_t pat;                           /* the pattern now: the main loop edits, the ISR plays */
static step_t *seq_step(uint32_t t, uint32_t i) { return &pat.st[t % NTRK][i % SEQ_STEPS]; }

/* main loop -> ISR */
static volatile uint32_t ctl_seq_run, ctl_seq_restart;
/* the tracks' start / stop: the main loop flips a bit of ctl_trk_req, the ISR takes it on the next downbeat (stopped:
 * at once) and flips the same bit of trk_req_seen; ctl_trk_on is the ISR's: which tracks play */
static volatile uint32_t ctl_trk_req, ctl_trk_on = (1u << NTRK) - 1u;
static uint32_t trk_req_seen;
static uint32_t trk_pending(void) { return (ctl_trk_req ^ trk_req_seen) & ((1u << NTRK) - 1u); }
/* the next pattern: the main loop reads it into pat_q and sets ctl_pat_swap; the ISR swaps it in on the next downbeat
 * (the one it leaves goes to pat_old, so its edits can be saved at the stop) */
static pattern_t pat_q __attribute__((section(".pool"))), pat_old __attribute__((section(".pool")));   /* (14 KB each: the pool) */
static volatile uint32_t ctl_pat_swap, ctl_pat_keep, ui_pat_swapped;   /* ctl_pat_swap: 1 waiting, 2 the ISR swaps, 0 */   /* keep: the one left goes to pat_old */
/* (OberVave 3) every pattern has its own four sounds: they change with it on the same downbeat (snd_q in, the ones
 * left to snd_old), so the new pattern's first note already plays its own instrument */
static patch_t snd_q[NTRK], snd_old[NTRK];
static volatile uint32_t ctl_snd_swap, ui_snd_swapped;   /* ui_: the sounds came with it (snd_old holds the ones left) */
/* ISR -> main loop: where each track is (step, and how far into it, Q8) */
static volatile uint32_t ui_seq_pos[NTRK], ui_seq_frac[NTRK], ui_seq_dur[NTRK], ui_seq_ticks, ui_seq_beat, ui_seq_bar_beat;

/* ---- a fresh pattern */
static void step_clear(step_t *s)
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD; i++) s->note[i] = 0;
    s->vel = 100;
    s->gate = 96;                               /* 76 % */
    s->ratch = 0;
    s->chance = 100;
    s->nudge = 0;
}
static void lane_clear(lane_t *l) { uint32_t i; l->param = AUTO_NONE; for (i = 0; i < SEQ_STEPS; i++) l->v[i] = AUTO_NONE; }
static void auto_init(pattern_t *p)
{
    uint32_t t, l;
    p->auto_magic = AUTO_MAGIC;
    for (t = 0; t < NTRK; t++) for (l = 0; l < NLANE; l++) lane_clear(&p->lane[t][l]);
}
static void pat_init(pattern_t *p)
{
    uint32_t i, t;
    p->magic = PAT_MAGIC;
    auto_init(p);
    for (t = 0; t < NTRK; t++) {
        for (i = 0; i < SEQ_STEPS; i++) step_clear(&p->st[t][i]);
        p->trk[t].len = 16;                     /* a bar of sixteenths each */
        p->trk[t].div = D_16;
        p->trk[t].swing = 50;
        p->trk[t].transp = 0;
        p->trk[t].mute = 0;
        p->trk[t].pad[0] = p->trk[t].pad[1] = 0;
    }
}
static void pat_factory(uint32_t n, pattern_t *p)   /* a pattern never saved: empty, four bars of sixteenths */
{
    (void)n;
    pat_init(p);
}

/* ---- (3.1) the transposition track: a fifth, lighter track that holds a transposition (in semitones) per step, with its
 * own length and note value (so it loops against the notes' tracks as a polymeter), and moves the tracks it targets as
 * it plays. It lives in the LAST lane of the last track (lane[NTRK-1][NLANE-1]) so that the pattern keeps its size: a
 * pattern saved by 3.0 loads as it was (that lane is empty), and 3.0 skips this lane (its param is >= NPARAM).
 *   lane.param  XP_LANE when the track is made (AUTO_NONE: not yet; anything else: an old recorded knob sits there)
 *   lane.pad[0] length in steps, 0 = off (1..255)      lane.pad[1] note value (D_4 ..)
 *   lane.pad[2] bits 0-3: the tracks A-D it moves (default all), bits 4-5: when a LIVE change goes in (0 now, 1 beat, 2 bar)
 *   lane.v[i]   transposition + XP_BIAS on step i; XP_NONE: that step changes nothing (the last value goes on) */
#define XP_LANE 0xFEu
#define XP_NONE AUTO_NONE
#define XP_BIAS 64
#define XP_MAXLEN 255u
#define XPL() (pat.lane[NTRK - 1u][NLANE - 1u])
static uint32_t xp_lane_free(void) { return XPL().param == AUTO_NONE || XPL().param == XP_LANE; }   /* the lane is ours to use */
static uint32_t xp_active(void) { return XPL().param == XP_LANE; }
static uint32_t xp_len(void) { return xp_active() ? XPL().pad[0] : 0u; }
static uint32_t xp_div(void) { return xp_active() ? XPL().pad[1] % NDIV : (uint32_t)D_4; }
static uint32_t xp_mask(void) { return xp_active() ? XPL().pad[2] & 15u : 15u; }
static uint32_t xp_quant(void) { return xp_active() ? (XPL().pad[2] >> 4 & 3u) % 3u : 0u; }
static void xp_make(void)                       /* (main loop) the track is made on first use, empty, off, all four tracks */
{
    lane_t *a = &XPL();
    if (a->param == XP_LANE || a->param != AUTO_NONE) return;
    lane_clear(a);
    a->param = XP_LANE; a->pad[0] = 0; a->pad[1] = D_4; a->pad[2] = 15;
}

/* ---- the player (ISR, per CTL block) */
#define NO_WAIT 0xFFFFFFFFu
typedef struct { uint32_t cnt, dur, step, sub, rat_n, rat_i, gate_at, nsnd, play, tie, wait, on_at, hold_end, frac; uint8_t snd[SEQ_CHORD]; } seqrun_t;   /* hold_end: a late note's end in the next step */
static seqrun_t sr[NTRK];
static uint32_t seq_was, seq_beat_cnt, seq_beat_n, count_left;

/* a note held on the keys while REC takes it (main loop -> ISR): the key owns it, the sequencer neither plays nor ends
 * it on that track (else the step just recorded, or the loop coming round, cut the long note being played) */
static volatile uint32_t ctl_live_hold[NTRK][4];
static int seq_live(uint32_t t, uint32_t n) { return ctl_live_hold[t % NTRK][n >> 5 & 3u] >> (n & 31u) & 1u; }
/* an arp take (REC, the arp on the track playing into it): the track's steps are not played meanwhile – you hear the arp
 * that is being written, not the last round of it on top */
static volatile uint32_t ctl_rec_mute;

static void seq_release_t(uint32_t t)
{
    seqrun_t *r = &sr[t];
    uint32_t i;
    for (i = 0; i < r->nsnd; i++) if (!seq_live(t, r->snd[i])) voice_release(t, r->snd[i], 1);
    r->nsnd = 0;
}
static uint32_t seq_note(const step_t *s, uint32_t i, int32_t transp)
{
    return (uint32_t)clampi((int32_t)s->note[i] + transp, 1, 127);
}
static void seq_sound(uint32_t t, const step_t *s, int32_t transp)
{
    seqrun_t *r = &sr[t];
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t n = seq_note(s, i, transp);
        if (seq_live(t, n) || ctl_rec_mute >> t & 1u) continue;
        TRACE('S', t, n);
        voice_trigger(t, n, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)n;
    }
}
static uint32_t seq_has(const step_t *s, uint32_t m, int32_t transp)   /* the step plays note m */
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i]; i++)
        if (seq_note(s, i, transp) == m) return 1;
    return 0;
}

/* a tie, note by note: what the new step plays again keeps sounding, the rest lets go, the new ones start */
static void seq_tie_into(uint32_t t, const step_t *s, int32_t transp, uint32_t play)
{
    seqrun_t *r = &sr[t];
    uint32_t i, j, keep = 0;
    for (i = j = 0; i < r->nsnd; i++) {
        if (play && seq_has(s, r->snd[i], transp)) { r->snd[j++] = r->snd[i]; keep++; }
        else if (!seq_live(t, r->snd[i])) voice_release(t, r->snd[i], 1);
    }
    r->nsnd = j;
    if (!play) return;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t m = seq_note(s, i, transp);
        for (j = 0; j < keep && r->snd[j] != m; j++) ;
        if (j < keep || seq_live(t, m) || ctl_rec_mute >> t & 1u) continue;
        voice_trigger(t, m, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)m;
    }
}

static void seq_from_top(uint32_t t)            /* track t: step 1 in this block */
{
    seqrun_t *r = &sr[t];
    seq_release_t(t);
    r->step = 0xFFFFFFFFu;
    r->cnt = r->dur = r->frac = 0;
    r->play = r->tie = 0;
    r->wait = NO_WAIT;
    r->hold_end = 0;
}

static void seq_take_requests(void)             /* the start / stop requests (on a downbeat, or at once when stopped) */
{
    uint32_t p = trk_pending(), t;
    if (!p) return;
    for (t = 0; t < NTRK; t++)
        if (p >> t & 1u) {
            if (ctl_trk_on >> t & 1u) { ctl_trk_on &= ~(1u << t); seq_release_t(t); }
            else { ctl_trk_on |= 1u << t; seq_from_top(t); }
        }
    trk_req_seen ^= p;
}

static int seq_phase(uint32_t *ph) { *ph = seq_beat_cnt; return ctl_seq_run && seq_was; }
static int seq_starting(void) { return ctl_seq_run && (!seq_was || ctl_seq_restart); }

/* where track t is in this block (the arp asks before the sequencer's turn: a step that begins in this block
 * counts as begun) – Q8: step * 256 + how far into it; 0xFFFFFFFF = not running */
static uint32_t seq_q8_now(uint32_t t)
{
    const seqrun_t *r = &sr[t % NTRK];
    uint32_t len = pat.trk[t % NTRK].len, step = r->step, cnt = r->cnt, dur = r->dur;
    if (!len || !ctl_seq_run) return 0xFFFFFFFFu;
    if (seq_starting() || step == 0xFFFFFFFFu || !dur) return 0;
    if (cnt >= dur) { cnt -= dur; step = step + 1u >= len ? 0u : step + 1u; }
    return (step % len) * 256u + (cnt * 256u / dur > 255u ? 255u : cnt * 256u / dur);
}
static void mx_swap_in(void)                    /* (ISR, inside the pattern swap) */
{
    uint32_t t;
    if (!ctl_mx_swap) return;
    for (t = 0; t < NPART; t++) {
        ctl_mix_lvl[t] = mx_q.lvl[t]; ctl_mix_cho[t] = mx_q.cho[t]; ctl_mix_dly[t] = mx_q.dly[t];
        ctl_mix_rev[t] = mx_q.rev[t]; ctl_mix_drv[t] = mx_q.drv[t]; ctl_mix_pan[t] = mx_q.pan[t];
    }
    ctl_mix_off = mx_q.off;
    ctl_dv_type = mx_q.dv_type; ctl_dv_amt = mx_q.dv_amt; ctl_dv_tone = mx_q.dv_tone; ctl_dv_mix = mx_q.dv_mix;
    ctl_m_vol = mx_q.m_vol; ctl_m_dj = mx_q.m_dj; ctl_m_drive = mx_q.m_drive; ctl_m_comp = mx_q.m_comp;
    ctl_mx_swap = 0;
}

/* the recorded knobs of track t on step i: into its sound (a lane being recorded right now is left to the hand) */
static volatile uint32_t ctl_auto_hold[NTRK];   /* main loop: bit l = lane l of track t is being turned */
static void seq_auto(uint32_t t, uint32_t i)
{
    uint32_t l;
    for (l = 0; l < NLANE; l++) {
        const lane_t *a = &pat.lane[t][l];
        uint8_t v = a->v[i % SEQ_STEPS];
        if (a->param == AUTO_NONE || v == AUTO_NONE || (ctl_auto_hold[t] >> l & 1u) || a->param >= NPARAM) continue;
        live_p[t].v[a->param] = v;
    }
}
/* (3.1) the transposition's player. ctl_xp_live: what the hand asks (the main loop's), taken at once, on a beat or on the
 * bar (xp_quant); ctl_xp_set: a value the main loop has just recorded, which the track holds from now on (the step it was
 * written on is already begun). A track plays its notes + its own transposition + (if the track is targeted) both. */
typedef struct { uint32_t cnt, dur, step, frac; } xprun_t;
static xprun_t xr;
static volatile int32_t ctl_xp_live, ctl_xp_set;
static volatile uint32_t ctl_xp_set_gen;
static uint32_t xp_set_seen, xp_bar_blk;        /* xp_bar_blk: this block begins a bar (the track starts only there: in step with the notes) */
static int32_t xp_live_ap, xp_trk;               /* ISR: the live offset in force, the track's value now */
static volatile int32_t ui_xp_now;              /* ISR -> UI: the sum the targeted tracks are moved by */
static volatile uint32_t ui_xp_pos, ui_xp_frac; /* the track's step, and how far into it (Q8) */
static void xp_from_top(void) { xr.cnt = xr.dur = xr.frac = 0; xr.step = 0xFFFFFFFFu; xp_trk = 0; }
static int32_t xp_for(uint32_t t) { return xp_mask() >> t & 1u ? xp_live_ap + xp_trk : 0; }
static void xp_commit(uint32_t beat, uint32_t bar)   /* the live change goes in: now, on the next beat, or on the bar */
{
    uint32_t m = xp_quant();
    if (xp_live_ap == ctl_xp_live) return;
    if (m == 0u || (m == 1u && beat) || (m == 2u && bar)) { xp_live_ap = ctl_xp_live; TRACE('L', m, (uint32_t)xp_live_ap); }
}
static void xp_tick(uint32_t q)                 /* the track's own steps (q: the quarter's length in frames) */
{
    const lane_t *a = &XPL();
    uint32_t len = a->param == XP_LANE ? a->pad[0] : 0u;
    if (!len) { xp_trk = 0; xr.step = 0xFFFFFFFFu; xr.cnt = xr.dur = xr.frac = 0; ui_xp_now = xp_live_ap; return; }
    if (xr.step == 0xFFFFFFFFu && !xp_bar_blk) {   /* made or switched on while playing: it waits for the next bar */
        ui_xp_pos = 0; ui_xp_frac = 0; ui_xp_now = xp_live_ap + xp_trk;
        return;
    }
    if (xr.cnt >= xr.dur) {
        uint32_t dv = DIV_PER_QUARTER[a->pad[1] % NDIV], base = q / dv;
        uint8_t v;
        xr.frac += q % dv;                      /* (the rest of q spread over the steps, as the tracks' steps) */
        if (xr.frac >= dv) { xr.frac -= dv; base++; }
        xr.cnt -= xr.dur;
        xr.step = xr.step + 1u >= len ? 0u : xr.step + 1u;
        xr.dur = base < CTL ? CTL : base;
        if (!xr.step) xp_trk = 0;               /* (every round starts from 0 unless step 1 says otherwise: each pass sounds the same) */
        v = a->v[xr.step];
        if (v != XP_NONE) xp_trk = (int32_t)v - XP_BIAS;
        TRACE('X', xr.step, (uint32_t)xp_trk);
    }
    xr.cnt += CTL;
    ui_xp_pos = xr.step;
    ui_xp_frac = xr.dur ? xr.cnt * 256u / xr.dur : 0u;
    ui_xp_now = xp_live_ap + xp_trk;
}

static void seq_tick(void)
{
    uint32_t t, q;
    if (ctl_xp_set_gen != xp_set_seen) { xp_set_seen = ctl_xp_set_gen; xp_trk = ctl_xp_set; }   /* (a recorded value: held from now) */
    if (!ctl_seq_run) {
        if (seq_was) { for (t = 0; t < NTRK; t++) seq_release_t(t); seq_was = 0; }
        seq_take_requests();                    /* (stopped: a pad switches its track at once) */
        xp_live_ap = ctl_xp_live;               /* (stopped: the hand's transposition at once, the track at its start) */
        xp_trk = 0; xr.step = 0xFFFFFFFFu; xr.cnt = xr.dur = xr.frac = 0;
        ui_xp_now = xp_live_ap;
        return;
    }
    if (!seq_was || ctl_seq_restart) {          /* start: step 1 of every track in this block */
        ctl_seq_restart = 0;
        seq_take_requests();
        for (t = 0; t < NTRK; t++) seq_from_top(t);
        xp_from_top();
        seq_beat_cnt = 0;
        seq_beat_n = 0;
        count_left = ctl_count ? 4u : 0u;
        seq_was = 1;
    }
    q = quarter_len();
    xp_bar_blk = 0;
    xp_commit(0u, 0u);                          /* (quantise NOW: in this block) */
    if (seq_beat_cnt < CTL) {                   /* a beat (in this block): the PLAY light, the click, the count-in; a downbeat: requests */
        if (ctl_count && !count_left) {         /* counted in: the tracks from the top, now */
            ctl_count = 0;
            seq_beat_n = 0;
            for (t = 0; t < NTRK; t++) seq_from_top(t);
        xp_from_top();
        }
        if (!ctl_count && seq_beat_n % BAR_BEATS == 0u) {   /* the bar's first beat: what waited for it */
            if (ctl_pat_swap == 1u) {           /* (the ISR is never interrupted by the main loop: no lock needed) */
                ctl_pat_swap = 2u;
                /* the next pattern (2 = being swapped in: the main loop leaves pat_q alone): the tracks from the top */
                if (ctl_pat_keep) pat_old = pat;
                pat = pat_q;
                if (ctl_snd_swap) { for (t = 0; t < NTRK; t++) { snd_old[t] = live_p[t]; live_p[t] = snd_q[t]; } ctl_snd_swap = 0; ui_snd_swapped = 1; }
                mx_swap_in();                   /* (its mixer, drive and master on the same downbeat) */
                RING_PUBLISH();
                ctl_pat_swap = 0;
                ui_pat_swapped++;
                for (t = 0; t < NTRK; t++) seq_from_top(t);
        xp_from_top();
            }
            seq_take_requests();
        }
        xp_bar_blk = !ctl_count && seq_beat_n % BAR_BEATS == 0u;
        xp_commit(1u, xp_bar_blk);              /* (on the beat; on the bar) */
        ui_seq_beat++;
        ui_seq_bar_beat = seq_beat_n % BAR_BEATS;
        if (ctl_click || ctl_count) click_start(seq_beat_n % BAR_BEATS == 0u);
        seq_beat_n++;
        if (count_left) count_left--;
        ui_seq_count = ctl_count ? count_left + 1u : 0u;
    }
    seq_beat_cnt += CTL;
    if (seq_beat_cnt >= q)                      /* the next beat: what is over keeps going (a DAW's quarter is any length) */
        seq_beat_cnt = seq_beat_cnt - q < CTL ? seq_beat_cnt - q : 0u;
    if (ctl_count) return;                      /* counting in: no steps yet */
    xp_tick(q);                                 /* (3.1) the transposition track, then every track with its share of it */
    for (t = 0; t < NTRK; t++) {
        seqrun_t *r = &sr[t];
        const seqtrk_t *k = &pat.trk[t];
        const step_t *s;
        uint32_t len = k->len;
        int32_t tp = (int32_t)k->transp + xp_for(t);
        if (!len || !(ctl_trk_on >> t & 1u)) { seq_release_t(t); r->tie = 0; r->wait = NO_WAIT; r->hold_end = 0; continue; }
        if (r->cnt >= r->dur) {                 /* the next step */
            uint32_t dv = DIV_PER_QUARTER[k->div % NDIV], base = q / dv, sw, carry = 0;
            r->frac += q % dv;                  /* (the rest of q spread over the steps: div steps are exactly a quarter) */
            if (r->frac >= dv) { r->frac -= dv; base++; }
            if (r->hold_end) { r->hold_end = 0; seq_release_t(t); }   /* (a carried note: over by now) */
            if (r->nsnd && r->wait == NO_WAIT && r->gate_at != 0xFFFFFFFFu) {   /* a late note (nudged) lasts into this step */
                uint32_t e = r->on_at + r->rat_i * r->sub + r->gate_at;
                if (e > r->dur) carry = e - r->dur;
            }
            r->cnt -= r->dur;
            r->step = r->step + 1u >= len ? 0u : r->step + 1u;
            sw = base * (uint32_t)(clampi(k->swing, 50, 75) - 50) / 50u;
            r->dur = r->step & 1u ? base - sw : base + sw;   /* swing: the first of each pair longer */
            if (r->dur < CTL) r->dur = CTL;
            s = seq_step(t, r->step);
            seq_auto(t, r->step);
            TRACE('T', t, r->step * 1000000u + (s->note[0] ? (uint32_t)s->note[0] * 1000u : 0u) + (s->note[1] ? 900u : 0u) + (s->gate >= GATE_TIE ? 1u : 0u));
            r->play = s->note[0] && !(s->ratch & STEP_OFF) && (s->chance >= 100u || xorshift(&rnd_state) % 100u < s->chance);
            r->wait = NO_WAIT;
            r->on_at = 0;
            if (r->play && s->nudge) {          /* later in the step: the old notes go (a late one at its end), the new ones when it is time */
                if (!carry) seq_release_t(t);
                r->on_at = (r->dur * s->nudge) >> 7;
                r->wait = r->on_at;
            } else if (r->tie)                  /* a tie: the notes the step plays again hold */
                seq_tie_into(t, s, tp, r->play);
            else {
                if (r->play) carry = 0;
                if (!carry) seq_release_t(t);
                if (r->play) seq_sound(t, s, tp);
            }
            r->hold_end = carry;
            r->rat_n = (s->ratch & 3u) + 1u;
            r->sub = (r->dur - r->on_at) / r->rat_n;
            r->rat_i = 0;
            r->tie = r->play && s->gate >= GATE_TIE && !s->nudge;
            {                                   /* the length: of a whole step (a ratchet's share), also for a late note */
                uint32_t g = r->dur / r->rat_n;
                r->gate_at = s->gate >= GATE_TIE ? 0xFFFFFFFFu : (uint32_t)clampi((int32_t)((g * (s->gate + 1u)) >> 7), CTL, (int32_t)(g > CTL ? g : CTL));
            }
            ui_seq_ticks++;
        } else {
            s = seq_step(t, r->step < len ? r->step : 0u);
            if (r->wait != NO_WAIT && r->cnt >= r->wait) { r->wait = NO_WAIT; r->hold_end = 0; seq_release_t(t); seq_sound(t, s, tp); }   /* a nudged step: now */
            else if (r->wait == NO_WAIT && r->play && r->rat_n > 1u && r->sub) {   /* a ratchet: the next hit */
                uint32_t i = (r->cnt - r->on_at) / r->sub;
                if (i > r->rat_i && i < r->rat_n) { r->rat_i = i; seq_release_t(t); seq_sound(t, s, tp); }
            }
        }
        if (r->hold_end && r->cnt >= r->hold_end) { r->hold_end = 0; seq_release_t(t); }   /* a late note from the step before: its end */
        if (r->nsnd && !r->hold_end && r->wait == NO_WAIT && r->gate_at != 0xFFFFFFFFu && r->cnt >= r->on_at && r->cnt - r->on_at - r->rat_i * r->sub >= r->gate_at)
            seq_release_t(t);
        r->cnt += CTL;
        ui_seq_dur[t] = r->dur;
        ui_seq_pos[t] = r->step;
        ui_seq_frac[t] = r->dur ? r->cnt * 256u / r->dur : 0u;
    }
}
