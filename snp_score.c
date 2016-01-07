//#define DEBUG

/*
 * Prunes quality based on snp calling score.
 * 
 * Bi-allelic consensus is computed (possibly as hom).
 * If call has high confidence, then bin to 2 qualities.
 * - high if base is one of the 1-2 consensus calls.
 * - low otherwise.
 *
 * Exceptions.
 * - Within 'D' bases of any read indel.
 * - Within 'D' bases of a soft-clip (possible clipped indel).
 * - Any sequence with mapping quality of <= M.
 * - Any column with high discrepancy, implying possibly tri-allelic.
 *
 */

/*
 * TODO: Het indels may mean we quantise the indel but not the
 * non-indel.  This gives a bias.  Need to treat both the same.
 *
 * Identify low complexity regions.  If we're preserving confidence
 * then it needs to be for all bases in that low-complexity section so
 * that local realignment doesn't change qualities.
 */


#define INDEL_DIST 40
#define QL 3
#define QM 25 // below => QL, else QH
#define QH 42
#define M 0

//#define STR_DIST 2
#define STR_DIST 1

#define MIN_QUAL 75
#define MIN_INDEL 100

//#define MIN_QUAL 30
//#define MIN_INDEL 50

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <math.h>
#include <float.h>

#include <htslib/sam.h>

#include "str_finder.h"

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

#ifndef ABS
#define ABS(a) ((a)>=0?(a):-(a))
#endif


//-----------------------------------------------------------------------------
// Binning

static int bin2[256];

void init_bins(void) {
    int i;
    for (i = 0; i < QM; i++)
	bin2[i] = QL;
    for (; i < 256; i++)
	bin2[i] = QH;
}

//-----------------------------------------------------------------------------
// Bits ripped out of gap5's consensus algorithm; an interim solution

#define CONS_NO_END_N   1
#define CONS_SCORES     2
#define CONS_DISCREP    4
#define CONS_COUNTS     8
#define CONS_ALL        15

#define CONS_MQUAL      16

typedef struct {
    /* the most likely base call - we never call N here */
    /* A=0, C=1, G=2, T=3, *=4 */
    int call;

    /* The most likely heterozygous base call */
    /* Use "ACGT*"[het / 5] vs "ACGT*"[het % 5] for the combination */
    int het_call;

    /* Log-odds values for A, C, G and T and gap. 5th is N, with 0 prob */
    /* scores[6] is score for het_call above */
    float scores[7];

    /* Single phred style call */
    unsigned char phred;

    /* Sequence depth */
    int depth;

    /* Individual base type counts */
    int counts[6];

    /* Discrepancy search score */
    float discrep;
} consensus_t;

#define P_HET 1e-6

#define LOG10        2.30258509299404568401
#define TENOVERLOG10 4.34294481903251827652

/* Sequencing technologies for seq_t.seq_tech; 5 bits, so max=31 */
#define STECH_UNKNOWN    0
#define STECH_SANGER     1
#define STECH_SOLEXA     2
#define STECH_SOLID      3
#define STECH_454        4
#define STECH_HELICOS    5
#define STECH_IONTORRENT 6
#define STECH_PACBIO     7
#define STECH_ONT        8
#define STECH_LAST       8 // highest value

double tech_undercall[] = {
    1.00, // unknown
    1.00, // sanger
    1.00, // solexa/illumina
    1.00, // solid
    1.00, // 454
    1.00, // helicos
    1.00, // iontorrent
    1.00, // pacbio
    1.63, // ont
};

static double prior[25];     /* Sum to 1.0 */
static double lprior15[15];  /* 15 combinations of {ACGT*} */

/* Precomputed matrices for the consensus algorithm */
static double pMM[9][101], p__[9][101], p_M[9][101], po_[9][101], poM[9][101];
static double poo[9][101], puu[9][101], pum[9][101], pmm[9][101];

static double e_tab_a[1002];
static double *e_tab = &e_tab_a[500];
static double e_log[501];

/*
 * Lots of confusing matrix terms here, so some definitions will help.
 *
 * M = match base
 * m = match pad
 * _ = mismatch
 * o = overcall
 * u = undercall
 *
 * We need to distinguish between homozygous columns and heterozygous columns,
 * done using a flat prior.  This is implemented by treating every observation
 * as coming from one of two alleles, giving us a 2D matrix of possibilities
 * (the hypotheses) for each and every call (the observation).
 *
 * So pMM[] is the chance that given a call 'x' that it came from the
 * x/x allele combination.  Similarly p_o[] is the chance that call
 * 'x' came from a mismatch (non-x) / overcall (consensus=*) combination.
 *
 * Examples with observation (call) C and * follows
 *
 *  C | A  C  G  T  *          * | A  C  G  T  * 
 *  -----------------	       ----------------- 
 *  A | __ _M __ __ o_	       A | uu uu uu uu um
 *  C | _M MM _M _M oM	       C | uu uu uu uu um
 *  G | __ _M __ __ o_	       G | uu uu uu uu um
 *  T | __ _M __ __ o_	       T | uu uu uu uu um
 *  * | o_ oM o_ o_ oo	       * | um um um um mm
 *
 * In calculation terms, the _M is half __ and half MM, similarly o_ and um.
 *
 * Relative weights of substitution vs overcall vs undercall are governed on a
 * per base basis using the P_OVER and P_UNDER scores (subst is 1-P_OVER-P_UNDER).
 *
 * The heterozygosity weight though is a per column calculation as we're
 * trying to model whether the column is pure or mixed. Hence this is done
 * once via a prior and has no affect on the individual matrix cells.
 */

static void consensus_init(double p_het) {
    int i, t;

    for (i = -500; i <= 500; i++)
    	e_tab[i] = exp(i);
    for (i = 0; i <= 500; i++)
	e_log[i] = log(i);

    // Heterozygous locations
    for (i = 0; i < 25; i++)
	prior[i] = p_het / 20;
    prior[0] = prior[6] = prior[12] = prior[18] = prior[24] = (1-p_het)/5;

    lprior15[0]  = log(prior[0]);
    lprior15[1]  = log(prior[1]*2);
    lprior15[2]  = log(prior[2]*2);
    lprior15[3]  = log(prior[3]*2);
    lprior15[4]  = log(prior[4]*2);
    lprior15[5]  = log(prior[6]);
    lprior15[6]  = log(prior[7]*2);
    lprior15[7]  = log(prior[8]*2);
    lprior15[8]  = log(prior[9]*2);
    lprior15[9]  = log(prior[12]);
    lprior15[10] = log(prior[13]*2);
    lprior15[11] = log(prior[14]*2);
    lprior15[12] = log(prior[18]);
    lprior15[13] = log(prior[19]*2);
    lprior15[14] = log(prior[24]);


    // Rewrite as new form
    for (t = STECH_UNKNOWN; t <= STECH_LAST; t++) {
	for (i = 1; i < 101; i++) {
	    double prob = 1 - pow(10, -i / 10.0);

//	    if (t == STECH_ONT)
//		prob = 0.85; // Fake FIXED prob for now

	    // May want to multiply all these by 5 so pMM[i] becomes close
	    // to -0 for most data. This makes the sums increment very slowly,
	    // keeping bit precision in the accumulator.
#if 0
	    double p_overcall[] = {
		0.001, // unknown
		0.010, // sanger
		0.001, // solexa/illumina
		0.001, // solid
		0.010, // 454
		0.010, // helicos
		0.010, // iontorrent
		0.010, // pacbio
		0.050, // ont
	    };

	    double p_undercall[] = {
		0.001, // unknown
		0.010, // sanger
		0.001, // solexa/illumina
		0.001, // solid
		0.010, // 454
		0.010, // helicos
		0.010, // iontorrent
		0.010, // pacbio
		0.280, // ont
	    };

	    double norm = (1-p_overcall[t])*prob + 3*((1-p_overcall[t])*(1-prob)/3)
		+ p_overcall[t]*(1-prob);
	    pMM[t][i] = log((1-p_overcall[t]) * prob /norm);
	    p__[t][i] = log((1-p_overcall[t]) * (1-prob)/3 /norm);
	    poo[t][i] = log((p_overcall[t]*(1-prob)) /norm);

	    p_M[t][i] = log((exp(pMM[t][i]) + exp(p__[t][i]))/2);
	    po_[t][i] = log((exp(p__[t][i]) + exp(poo[t][i]))/2);
	    poM[t][i] = log((exp(pMM[t][i]) + exp(poo[t][i]))/2);

	    // [t]* observation vs base
	    norm = p_undercall[t]*(1-prob)*4 + (1-p_undercall[t])*prob;
	    puu[t][i] = log((p_undercall[t] * (1-prob)) /norm);
	    pmm[t][i] = log((1-p_undercall[t])*prob /norm);
	    pum[t][i] = log((exp(puu[t][i]) + exp(pmm[t][i]))/2);
#else
	    //prob = 1-(1-prob)*2; if (prob < 0.1) prob = 0.1; // Fudge

	    pMM[t][i] = log(prob/5);
	    p__[t][i] = log((1-prob)/20);
	    p_M[t][i] = log((exp(pMM[t][i]) + exp(p__[t][i]))/2);

	    puu[t][i] = p__[t][i];

	    poM[t][i] = p_M[t][i] *= tech_undercall[t];
	    po_[t][i] = p__[t][i] *= tech_undercall[t];
	    poo[t][i] = p__[t][i] *= tech_undercall[t];
	    pum[t][i] = p_M[t][i] *= tech_undercall[t];
	    pmm[t][i] = pMM[t][i] *= tech_undercall[t];
#endif
	}

	pMM[t][0] = pMM[t][1];
	p__[t][0] = p__[t][1];
	p_M[t][0] = p_M[t][1];

	pmm[t][0] = pmm[t][1];
	poo[t][0] = poo[t][1];
	po_[t][0] = po_[t][1];
	poM[t][0] = poM[t][1];
	puu[t][0] = puu[t][1];
	pum[t][0] = pum[t][1];
    }
}

/* 
 * See "A Fast, Compact Approximation of the Exponential Function"
 * by NN. Schraudolph, Neural Computation, 1999
 */
#if 0
static inline double fast_exp(double y) {
    union {
	double d;
	int i, j;
    } x;

    x.i = 0;
    x.j = 1512775 * y + 1072632447;

    return x.d;
}

static inline double fast_log(double y) {
    union {
	double d;
	int i, j;
    } x;
    
    x.d = y;
    return (x.j - 1072632447.0) / 1512775;
}
#endif

#if 1
static inline double fast_exp(double y) {
    if (y < -500)
	y = -500;
    if (y > 500)
	y = 500;

    //    printf("%f => %g %g\n", y, exp(y), e_tab[(int)y]);

    return e_tab[(int)y];
}
#endif

//#define fast_exp exp
#define fast_log log

/*
 * As per calculate_consensus_bit_het but for a single pileup column.
 */
int calculate_consensus_pileup(int flags,
			       const bam_pileup1_t *p,
			       int np,
			       consensus_t *cons) {
    int i, j;
    static int init_done =0;
    static double q2p[101];
    double min_e_exp = DBL_MIN_EXP * log(2) + 1;

    double S[15] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    double sumsC[6] = {0,0,0,0,0,0}, sumsE = 0;
    int depth = 0;

    /* Map the 15 possible combinations to 1-base or 2-base encodings */
    static int map_sing[15] = {0, 5, 5, 5, 5,
			          1, 5, 5, 5,
			             2, 5, 5,
			                3, 5,
			                   4};
    static int map_het[15] = {0,  1,  2,  3,  4,
			          6,  7,  8,  9,
			             12, 13, 14,
			                 18, 19,
			                     24};

    if (!init_done) {
	init_done = 1;
	consensus_init(P_HET);

	for (i = 0; i <= 100; i++) {
	    q2p[i] = pow(10, -i/10.0);
	}
    }

    /* Currently always needed for the N vs non-N evaluation */
    flags |= CONS_COUNTS;

    /* Initialise */
    if (flags & CONS_COUNTS) {
	cons->counts[0] = 0;
	cons->counts[1] = 0;
	cons->counts[2] = 0;
	cons->counts[3] = 0;
	cons->counts[4] = 0;
	cons->counts[5] = 0;
    }

    /* Accumulate */
    int n;
    //printf("-----\n");

    // FIXME: also seed with unknown alleles so low coverage data is
    // less confident.

    for (n = 0; n < np; n++) {
	bam1_t *b = p[n].b;
	uint8_t base = bam_seqi(bam_get_seq(b), p[n].qpos);
	uint8_t qual = bam_get_qual(b)[p[n].qpos];
	const int stech = STECH_SOLEXA;
	// =ACM GRSV TWYH KDBN
	static int L[16] = {
	    5,0,1,5, 2,5,5,5, 3,5,5,5, 5,5,5,5
	};

	// convert from sam base to acgt*n order.
	base = L[base];
	if (p[n].is_del) base = 4;

	double MM, __, _M, qe;

	// Correction for mapping quality.  Maybe speed up via lookups?
	// Cannot nullify mapping quality completely.  Lots of (true)
	// SNPs means low mapping quality.  (Ideally need to know
	// hamming distance to next best location.)

	if (flags & CONS_MQUAL) {
	    //double _p = 1-pow(10, -qual/10.0);
	    //double _m = 1-pow(10, -(b->core.qual+.1)/10.0);
	    double _p = 1-pow(10, -(qual/3+.1)/10.0);
	    double _m = 1-pow(10, -(b->core.qual/3+.1)/10.0);

	    //printf("%c %d -> %d, %f %f\n", "ACGT*N"[base], qual, (int)(-TENOVERLOG10 * log(1-(_m * _p + (1 - _m)/4))), _p, _m);
	    qual = -TENOVERLOG10 * log(1-(_m * _p + (1 - _m)/4));
	}

	// FIXME: try with and without modified qual and require both
	// to be certain. Discrepancy between them implies suspect calling?

	/* Quality 0 should never be permitted as it breaks the math */
	if (qual < 1)
	    qual = 1;

	__ = p__[stech][qual];
	MM = pMM[stech][qual];
	_M = p_M[stech][qual];

	if (flags & CONS_DISCREP) {
	    qe = q2p[qual];
	    sumsE += qe;
	    sumsC[base] += 1 - qe;
	}

	if (flags & CONS_COUNTS)
	    cons->counts[base]++;

	switch (base) {
	case 0:
	    S[0] += MM; S[1 ]+= _M; S[2 ]+= _M; S[3 ]+= _M; S[4 ]+= _M;
	                S[5 ]+= __; S[6 ]+= __; S[7 ]+= __; S[8 ]+= __;
			            S[9 ]+= __; S[10]+= __; S[11]+= __; 
				                S[12]+= __; S[13]+= __; 
						            S[14]+= __;
	    break;

	case 1:
	    S[0] += __; S[1 ]+= _M; S[2 ]+= __; S[3 ]+= __; S[4 ]+= __;
	                S[5 ]+= MM; S[6 ]+= _M; S[7 ]+= _M; S[8 ]+= _M;
			            S[9 ]+= __; S[10]+= __; S[11]+= __; 
				                S[12]+= __; S[13]+= __; 
						            S[14]+= __;
	    break;

	case 2:
	    S[0] += __; S[1 ]+= __; S[2 ]+= _M; S[3 ]+= __; S[4 ]+= __;
	                S[5 ]+= __; S[6 ]+= _M; S[7 ]+= __; S[8 ]+= __;
			            S[9 ]+= MM; S[10]+= _M; S[11]+= _M; 
				                S[12]+= __; S[13]+= __; 
						            S[14]+= __;
	    break;

	case 3:
	    S[0] += __; S[1 ]+= __; S[2 ]+= __; S[3 ]+= _M; S[4 ]+= __;
	                S[5 ]+= __; S[6 ]+= __; S[7 ]+= _M; S[8 ]+= __;
			            S[9 ]+= __; S[10]+= _M; S[11]+= __; 
				                S[12]+= MM; S[13]+= _M; 
						            S[14]+= __;
	    break;

	case 4:
	    S[0] += __; S[1 ]+= __; S[2 ]+= __; S[3 ]+= __; S[4 ]+= _M;
	                S[5 ]+= __; S[6 ]+= __; S[7 ]+= __; S[8 ]+= _M;
			            S[9 ]+= __; S[10]+= __; S[11]+= _M; 
				                S[12]+= __; S[13]+= _M; 
						            S[14]+= MM;
	    break;

	case 5: /* N => equal weight to all A,C,G,T but not a pad */
	    S[0] += MM; S[1 ]+= MM; S[2 ]+= MM; S[3 ]+= MM; S[4 ]+= _M;
	                S[5 ]+= MM; S[6 ]+= MM; S[7 ]+= MM; S[8 ]+= _M;
			            S[9 ]+= MM; S[10]+= MM; S[11]+= _M; 
				                S[12]+= MM; S[13]+= _M; 
						            S[14]+= __;
	    break;
	}

	depth++;
    }


    /* and speculate */
    {
	double shift, max, max_het, norm[15];
	int call = 0, het_call = 0, ph;
	double tot1, tot2;

	/*
	 * Scale numbers so the maximum score is 0. This shift is essentially 
	 * a multiplication in non-log scale to both numerator and denominator,
	 * so it cancels out. We do this to avoid calling exp(-large_num) and
	 * ending up with norm == 0 and hence a 0/0 error.
	 *
	 * Can also generate the base-call here too.
	 */
	shift = -DBL_MAX;
	max = -DBL_MAX;
	max_het = -DBL_MAX;
	for (j = 0; j < 15; j++) {
	    S[j] += lprior15[j];
	    if (shift < S[j]) {
		shift = S[j];
		//het_call = j;
	    }

	    /* Only call pure AA, CC, GG, TT, ** for now */
	    if (j != 0 && j != 5 && j != 9 && j != 12 && j != 14) {
		if (max_het < S[j]) {
		    max_het = S[j];
		    het_call = j;
		}
		continue;
	    }

	    if (max < S[j]) {
		max = S[j];
		call = j;
	    }
	}

	/*
	 * Shift and normalise.
	 * If call is, say, b we want p = b/(a+b+c+...+n), but then we do
	 * p/(1-p) later on and this has exceptions when p is very close
	 * to 1.
	 *
	 * Hence we compute b/(a+b+c+...+n - b) and
	 * rearrange (p/norm) / (1 - (p/norm)) to be p/norm2.
	 */
	for (j = 0; j < 15; j++) {
	    S[j] -= shift;
	    if (S[j] > min_e_exp) {
		//S[j] = exp(S[j]);
		S[j] = fast_exp(S[j]);
	    } else {
		S[j] = DBL_MIN;
	    }
	    norm[j] = 0;
	}

	tot1 = tot2 = 0;
	for (j = 0; j < 15; j++) {
	    norm[j]    += tot1;
	    norm[14-j] += tot2;
	    tot1 += S[j];
	    tot2 += S[14-j];
	}

	/* And store result */
	if (depth && depth != cons->counts[5] /* all N */) {
	    double m;

	    cons->depth = depth;

	    cons->call     = map_sing[call];
	    if (norm[call] == 0) norm[call] = DBL_MIN;
	    ph = -TENOVERLOG10 * fast_log(norm[call]) + .5;
	    cons->phred = ph > 255 ? 255 : (ph < 0 ? 0 : ph);

	    cons->het_call = map_het[het_call];
	    if (norm[het_call] == 0) norm[het_call] = DBL_MIN;
	    ph = TENOVERLOG10 * (fast_log(S[het_call]) - fast_log(norm[het_call])) + .5;
	    cons->scores[6] = ph;

	    if (flags & CONS_SCORES) {
		/* AA */
		if (norm[0] == 0) norm[0] = DBL_MIN;
		ph = TENOVERLOG10 * (fast_log(S[0]) - fast_log(norm[0])) + .5;
		cons->scores[0] = ph;

		/* CC */
		if (norm[5] == 0) norm[5] = DBL_MIN;
		ph = TENOVERLOG10 * (fast_log(S[5]) - fast_log(norm[5])) + .5;
		cons->scores[1] = ph;

		/* GG */
		if (norm[9] == 0) norm[9] = DBL_MIN;
		ph = TENOVERLOG10 * (fast_log(S[9]) - fast_log(norm[9])) + .5;
		cons->scores[2] = ph;

		/* TT */
		if (norm[12] == 0) norm[12] = DBL_MIN;
		ph = TENOVERLOG10 * (fast_log(S[12]) - fast_log(norm[12])) + .5;
		cons->scores[3] = ph;

		/* ** */
		if (norm[14] == 0) norm[14] = DBL_MIN;
		ph = TENOVERLOG10 * (fast_log(S[14]) - fast_log(norm[14])) + .5;
		cons->scores[4] = ph;

		/* N */
		cons->scores[5] = 0; /* N */
	    }

	    /* Compute discrepancy score */
	    if (flags & CONS_DISCREP) {
		m = sumsC[0]+sumsC[1]+sumsC[2]+sumsC[3]+sumsC[4];
		double c;
		if (cons->scores[6] > 0)
		    c = sumsC[cons->het_call%5] + sumsC[cons->het_call/5];
		else
		    c = sumsC[cons->call];;
		cons->discrep = (m-c)/sqrt(m);
//		printf("Discrep = %f,  %f %f %f %f %f\n", cons->discrep,
//		       sumsC[0], sumsC[1], sumsC[2], sumsC[3], sumsC[4]);
//		if (cons->discrep > 1)
//		    printf("XYZZY\n");
	    }
	} else {
	    cons->call = 5; /* N */
	    cons->het_call = 0;
	    cons->scores[0] = 0;
	    cons->scores[1] = 0;
	    cons->scores[2] = 0;
	    cons->scores[3] = 0;
	    cons->scores[4] = 0;
	    cons->scores[5] = 0;
	    cons->scores[6] = 0;
	    cons->phred = 0;
	    cons->depth = 0;
	    cons->discrep = 0;
	}
    }

    return 0;
}

//-----------------------------------------------------------------------------
// Doubly-sorted list of bam sequences

// Sorted list of bam1_t objects.
// These are doubly linked lists sorted on both start and end coords
// to permit quick updating.
typedef struct bam_sorted_item {
    struct bam_sorted_item *s_next, *s_prev; // sorted by start coord
    bam1_t *b;
    uint64_t id;
    int end_pos;
} bam_sorted_item;

typedef struct bam_sorted_list {
    bam_sorted_item *s_head, *s_tail; // start coord
} bam_sorted_list;

bam_sorted_list *bam_sorted_list_new(void) {
    bam_sorted_list *bl = calloc(1, sizeof(bam_sorted_list));
    if (!bl)
	return NULL;
	
    return bl;
}

bam_sorted_item *bam_sorted_item_new(void) {
    return calloc(1, sizeof(bam_sorted_item));
}

#ifdef DEBUG
// Slow debugging function
void validate_bam_list(bam_sorted_list *bl) {
    bam_sorted_item *t, *l;

    t = bl->s_head;
    l = NULL;
    while (t) {
	if (t->s_next)
	    assert(t->s_next->s_prev == t);
	if (t->s_prev)
	    assert(t->s_prev->s_next == t);
	assert(!l || t->b->core.pos >= l->b->core.pos);
	l = t;
	t = t->s_next;
    }
    assert(bl->s_tail == l);
}
#endif

/*
 * Inserts a bam structure into the bam_sorted_list, in positional
 * order.  Assumes data arrives in (start) positional order for
 * efficiency, but still works if this is not true.
 *
 * Returns the newly created bam_sorted_list element on success.
 *         NULL on failure.
 */
bam_sorted_item *insert_bam_list2(bam_sorted_list *bl, bam_sorted_item *ele) {
    bam_sorted_item *l, *r;
    bam1_t *b = ele->b;

    // start coord. l/r will be left/right of b
    l = bl->s_tail; r = NULL;
    while (l && l->b->core.pos > b->core.pos)
	r = l, l = l->s_prev;
    while (l && l->b->core.pos == b->core.pos && l->id > ele->id)
	r = l, l = l->s_prev;
    ele->s_prev = l;
    ele->s_next = r;

    if (l)
	l->s_next = ele;
    else
	bl->s_head = ele;

    if (r)
	r->s_prev = ele;
    else
	bl->s_tail = ele;

#ifdef DEBUG
    validate_bam_list(bl);
#endif

    return ele;
}

bam_sorted_item *insert_bam_list_id(bam_sorted_list *bl, bam1_t *b, uint64_t id) {
    bam_sorted_item *ele = bam_sorted_item_new();
    if (!ele)
	return NULL;
    
    ele->b = b;
    ele->id = id;
    ele->end_pos = bam_endpos(ele->b);

    return insert_bam_list2(bl, ele);
}

static uint64_t global_id = 0;
bam_sorted_item *insert_bam_list(bam_sorted_list *bl, bam1_t *b) {
    return insert_bam_list_id(bl, b, global_id++);
}

/*
 * Removes an item from the bam_sorted_list.
 */
void remove_bam_list(bam_sorted_list *bl, bam_sorted_item *ele) {
    // Start coord
    if (ele->s_prev)
	ele->s_prev->s_next = ele->s_next;
    else
	bl->s_head = ele->s_next;

    if (ele->s_next)
	ele->s_next->s_prev = ele->s_prev;
    else
	bl->s_tail = ele->s_prev;

    ele->s_prev = ele->s_next = NULL;
}

void flush_bam_list(bam_sorted_list *bl, int before, samFile *out, bam_hdr_t *header) {
    bam_sorted_item *bi_next = NULL, *bi;
    for (bi = bl->s_head; bi; bi = bi_next) {
	bi_next = bi->s_next;

	if (bi->end_pos >= before)
	    break;

	sam_write1(out, header, bi->b);
	bam_destroy1(bi->b);
	remove_bam_list(bl, bi);
    }
}

//-----------------------------------------------------------------------------
// Main pileup iterator

typedef struct {
    // sam, bam AND hts! All the weasles in a single bag :)
    samFile *fp;
    bam_hdr_t *header;
    hts_itr_t *iter;
    bam1_t *b_dup_list;
} pileup_cd;

int pileup_callback(void *vp, bam1_t *b) {
    pileup_cd *cd = (pileup_cd *)vp;
    return (cd->iter)
	? sam_itr_next(cd->fp, cd->iter, b)
	: sam_read1(cd->fp, cd->header, b);
}

// Turns an absolute reference position into a relative query position within the seq.
int ref2query_pos(bam1_t *b, int pos) {
     uint32_t *cig = bam_get_cigar(b);
     int i, n = b->core.n_cigar;
     int p = b->core.pos, q = 0;

     for (i = 0; i < n; i++) {
 	int op = bam_cigar_op(cig[i]);
 	int op_len = bam_cigar_oplen(cig[i]);
	if (p + ((bam_cigar_type(op) & 2) ? op_len : 0) < pos) {
	    if (bam_cigar_type(op) & 1) // query
		q += op_len;
	    if (bam_cigar_type(op) & 2) // ref
		p += op_len;
	    continue;
	}

	if (bam_cigar_type(op) & 1) // query
	    q += (pos - p); // consume partial op_len

	return q >= 0 ? q : 0;
     }

     return q;
}


// // Masks any base within D bases of an indel cigar operator
// void mask_indels(bam1_t *b) {
//     uint32_t *cig = bam_get_cigar(b);
//     int i, n = b->core.n_cigar, q;
// 
//     for (i = q = 0; i < n; i++) {
// 	int op = bam_cigar_op(cig[i]);
// 	int oplen = bam_cigar_oplen(cig[i]);
// 
// 	if (op == BAM_CINS || op == BAM_CDEL) {
// 	    uint8_t *qual = bam_get_qual(b);
// 	    int x, x_s = q-INDEL_DIST, x_e = q+oplen+INDEL_DIST;
// 	    if (x_s < 0) x_s = 0;
// 	    if (x_e > b->core.l_qseq) x_e = b->core.l_qseq;
// 	    for (x = x_s; x < x_e; x++)
// 		qual[x] |= 0x80;
// 	}
// 	if (bam_cigar_type(op) & 1)
// 	    q += oplen;
//     }
// }

// Masks any base within D bases of a soft-clip. Hard too?
void mask_clips(bam1_t *b) {
    uint32_t *cig = bam_get_cigar(b);
    int i, n = b->core.n_cigar, q;

    for (i = q = 0; i < n; i++) {
	int op = bam_cigar_op(cig[i]);
	int oplen = bam_cigar_oplen(cig[i]);

	if (op == BAM_CSOFT_CLIP) {
	    uint8_t *qual = bam_get_qual(b);
	    int x, x_s = q-INDEL_DIST, x_e = q+oplen+INDEL_DIST;
	    if (x_s < 0) x_s = 0;
	    if (x_e > b->core.l_qseq) x_e = b->core.l_qseq;
	    for (x = x_s; x < x_e; x++)
		qual[x] |= 0x80;
	}
	if (bam_cigar_type(op) & 1)
	    q += oplen;
    }
}

// Extend min/max reference positions based on any STRs at apos/rpos (abs/rel)
// into seq b.  This is used to find the extents over which we may wish to
// preserve scores given something important is going on at apos (typically
// indel).
void mask_LC_regions(bam1_t *b, int apos, int rpos, int *min_pos, int *max_pos) {
    int len = b->core.l_qseq;
    char *seq = malloc(len);
    int i;

    for (i = 0; i < len; i++)
	seq[i] = seq_nt16_str[bam_seqi(bam_get_seq(b), i)];

    rep_ele *reps = find_STR(seq, len, 0), *elt, *tmp;

    DL_FOREACH_SAFE(reps, elt, tmp) {
	if (!(rpos+STR_DIST >= elt->start && rpos-STR_DIST <= elt->end)) {
	    //fprintf(stderr, "SKIP rpos %d, apos %d:\t%2d .. %2d %.*s\n",
	    //	    rpos, apos,
	    //	    elt->start, elt->end,
	    //	    elt->end - elt->start+1, &seq[elt->start]);
	    DL_DELETE(reps, elt);
	    free(elt);
	    continue;
	}
	
	//fprintf(stderr, "%d:\t%2d .. %2d %.*s\n",
	//	apos,
	//	elt->start, elt->end,
	//	elt->end - elt->start+1, &seq[elt->start]);

	if (*min_pos > apos + elt->start - rpos - STR_DIST)
	    *min_pos = apos + elt->start - rpos - STR_DIST;
	if (*max_pos < apos + elt->end - rpos+1 + STR_DIST)
	    *max_pos = apos + elt->end - rpos+1 + STR_DIST;

	DL_DELETE(reps, elt);
	free(elt);
    }    

    free(seq);
}

// // Masks low-complexity data as pos and either side by as far as it extends.
// void mask_LC(bam1_t *bdest, bam1_t *bsrc, int pos) {
//     int len = bdest->core.l_qseq;
//     char *seq = malloc(len);
//     int i;
// 
//     pos -= bdest->core.pos;
// 
//     for (i = 0; i < len; i++)
// 	seq[i] = seq_nt16_str[bam_seqi(bam_get_seq(bsrc), i)];
// 
//     rep_ele *reps = find_STR(seq, len, 0), *elt, *tmp;
// 
//     DL_FOREACH_SAFE(reps, elt, tmp) {
// 	if (!(pos >= elt->start && pos <= elt->end)) {
// 	    DL_DELETE(reps, elt);
// 	    free(elt);
// 	    continue;
// 	}
// 	
// 	//fprintf(stderr, "%d:\t%2d .. %2d %.*s\n",
// 	//	bsrc->core.pos + pos,
// 	//	elt->start, elt->end,
// 	//	elt->end - elt->start+1, &seq[elt->start]);
// 	
// 	int x;
// 	//uint8_t *qual_src  = bam_get_qual(bsrc);
// 	uint8_t *qual_dest = bam_get_qual(bdest);
// 	for (x = MAX(elt->start - STR_DIST, 0);
// 	     x <= MIN(elt->end + STR_DIST, len-1);
// 	     x++) {
// 	    //qual_dest[x] = qual_src[x] | 0x80;
// 	    qual_dest[x] = 11 | 0x80;
// 	}
// 
// 	DL_DELETE(reps, elt);
// 	free(elt);
//     }    
// 
//     free(seq);
// }

int main(int argc, char **argv) {
    samFile *in, *out = NULL;
    bam_plp_t p_iter;
    int tid, pos;
    int n_plp;
    const bam_pileup1_t *plp;
    bam_hdr_t *header;
    pileup_cd cd;
    hts_itr_t *h_iter = NULL;
    bam_sorted_list *bl = bam_sorted_list_new();
    bam_sorted_list *b_hist = bam_sorted_list_new();

    init_bins();

    if (argc < 2) {
	fprintf(stderr, "Usage: indel_only SAM/BAM/CRAM-file [region]\n");
	return 1;
    }

    if (!(in = sam_open(argv[1], "rb"))) {
	perror(argv[1]);
	return 1;
    }

    if (!(out = sam_open("-", "wc"))) {
	//if (!(out = sam_open("-", "w"))) {
	perror("(stdout)");
	return 1;
    }

    hts_set_opt(in, HTS_OPT_NTHREADS, 2);
    hts_set_opt(out, HTS_OPT_NTHREADS, 2);

    if (!(header = sam_hdr_read(in))) {
	fprintf(stderr, "Failed to read file header\n");
	return 1;
    }
    if (out && sam_hdr_write(out, header) != 0) {
	fprintf(stderr, "Failed to write file header\n");
	return 1;
    }

    if (argc > 2) {
        hts_idx_t *idx = sam_index_load(in, argv[1]);
	h_iter = idx ? sam_itr_querys(idx, header, argv[2]) : NULL;
	if (!h_iter || !idx) {
	    fprintf(stderr, "Failed to load index and/or parse iterator.\n");
	    return 1;
	}
	hts_idx_destroy(idx);
    }

    cd.fp = in;
    cd.header = header;
    cd.iter = h_iter;

    p_iter = bam_plp_init(pileup_callback, &cd);
    int min_pos = INT_MAX, max_pos = 0;
    int min_pos2 = INT_MAX, max_pos2 = 0;

    while ((plp = bam_plp_auto(p_iter, &tid, &pos, &n_plp))) {
	int i, preserve = 0, indel = 0;
	unsigned char base;

	if (pos > max_pos2) {
	    min_pos2 = min_pos = INT_MAX;
	    max_pos2 = max_pos = 0; // beyond region to preserve quals.
	}

	if (h_iter) {
	    if (pos < h_iter->beg)
		continue;
	    if (pos >= h_iter->end)
		break;
	}

	consensus_t cons_g5_A, cons_g5_B;
	calculate_consensus_pileup(CONS_ALL,
				   plp, n_plp, &cons_g5_A);
	calculate_consensus_pileup(CONS_ALL | CONS_MQUAL,
				   plp, n_plp, &cons_g5_B);

#ifdef DEBUG
	printf("Depth %d\t%d\t%d\t", tid, pos+1, n_plp);
#endif
	int call1, call2; // samtools numerical =ACMGRSVTWYHKDBN

	if (cons_g5_A.scores[6] > 0) {
	    call1 = 1<<(cons_g5_A.het_call / 5);
	    call2 = 1<<(cons_g5_A.het_call % 5);
	} else {
	    call1 = call2 = 1<<cons_g5_A.call;
	}

#ifdef DEBUG
	if (cons_g5_A.scores[6] > 0) {
	    printf("%c/%c %4d\t",
	    	   "ACGT*"[cons_g5_A.het_call / 5], "ACGT*"[cons_g5_A.het_call % 5],
	    	   (int)cons_g5_A.scores[6]);
	} else {
	    printf("%c   %4d\t",
	    	   "ACGT*N"[cons_g5_A.call],
	    	   cons_g5_A.phred);
	    call1 = call2 = 1<<cons_g5_A.call;
	}

	if (cons_g5_B.scores[6] > 0) {
	    printf("%c/%c %4d\t",
		   "ACGT*"[cons_g5_B.het_call / 5], "ACGT*"[cons_g5_B.het_call % 5],
		   (int)cons_g5_B.scores[6]);
	} else {
	    printf("%c   %4d\t",
		   "ACGT*N"[cons_g5_B.call],
		   cons_g5_B.phred);
	}
#endif

	int hA = cons_g5_A.scores[6] > 0
	    ? cons_g5_A.het_call
	    : cons_g5_A.call * 5 + cons_g5_A.call;
	int sA = cons_g5_A.scores[6] > 0
	    ? cons_g5_A.scores[6]
	    : cons_g5_A.phred;
	int hB = cons_g5_B.scores[6] > 0
	    ? cons_g5_B.het_call
	    : cons_g5_B.call * 5 + cons_g5_B.call;
	int sB = cons_g5_B.scores[6] > 0
	    ? cons_g5_B.scores[6]
	    : cons_g5_B.phred;
	preserve = (hA != hB || sA < MIN_QUAL || sB < MIN_QUAL);
	preserve |= cons_g5_A.discrep >= 1 || cons_g5_B.discrep >= 1;

#ifdef DEBUG
	if (preserve) {
	    printf("*\t");
	} else {
	    printf("\t");
	}
#endif
	int left_most = n_plp ? plp[0].b->core.pos : 0;
	for (i = 0; i < n_plp; i++) {
	    // FIXME: only do this if the indel isn't VERY obvious.
	    // Eg a pileup of 30 reads with 1 single read having an over or
	    // under-call doesn't require full qualities.  We mainly need to
	    // store the indel quals if the indel could be heterozygous or
	    // a homozygous indel.
	    if ((plp[i].indel || plp[i].is_del)
		&& (sA < MIN_INDEL || sB < MIN_INDEL)) {
		if (indel < ABS(plp[i].indel) + plp[i].is_del)
		    indel = ABS(plp[i].indel) + plp[i].is_del;

		mask_LC_regions(plp[i].b, pos, plp[i].qpos+1, &min_pos, &max_pos);
		mask_LC_regions(plp[i].b, pos+indel, plp[i].qpos+1, &min_pos, &max_pos);
		if (min_pos > pos) min_pos = pos;
		if (max_pos < pos) max_pos = pos;

		// Extra growth, paranoia.
		//min_pos2 = pos - (pos-min_pos)*1.1;
		//max_pos2 = pos + (max_pos-pos)*1.1;
	    }

	    //if (min_pos != INT_MAX || indel)
	    //	fprintf(stderr, "%d..%d: Mask region = %d..%d\n", pos-1, pos+indel+1, min_pos, max_pos);
	}

	bam_sorted_item *bi = bl->s_head;
	for (i = 0; i < n_plp; i++) {
	    // b is unedited bam in pileup.
	    // b2 is edited bam in bam_sorted_list, for output
	    bam1_t *b = plp[i].b, *b2;
	    uint8_t *qual;

	    // At start, mark indel bases.
	    // Similarly low mqual if desired.
	    if (plp[i].is_head || !bi) {
		// !bi because we're maybe starting part way through due to a
		// !region query.
		b2 = insert_bam_list(bl, bam_dup1(b))->b;

		//mask_indels(b2);
		//mask_clips(b2);
		if (b->core.qual <= M) {
		    int x;
		    for (x = 0; x < b->core.l_qseq; x++)
			bam_get_qual(b2)[x] |= 0x80;
		}
	    } else {
		b2 = bi->b;
		bi = bi->s_next;
	    }

	    // Assert b2 & b are same object.
	    assert(strcmp(bam_get_qname(b), bam_get_qname(b2)) == 0);

	    qual = &bam_get_qual(b2)[plp[i].qpos];
	    base = bam_seqi(bam_get_seq(b), plp[i].qpos);

#ifdef DEBUG
	    putchar(seq_nt16_str[base]);
#endif

	    if (indel) {
		// New indel, so backfill to low-complexity range.
		int x;
		//fprintf(stderr, "pos %d indel %d, min/max_pos %d/%d, core pos %d, qpos %d => %d\n",
		//	pos, indel, min_pos2, max_pos2, b->core.pos, plp[i].qpos, ref2query_pos(b, min_pos2));
		for (x = ref2query_pos(b, min_pos2); x <= plp[i].qpos; x++) {
		    //bam_get_qual(b2)[x] = 11 | 0x80;
		    bam_get_qual(b2)[x] = bam_get_qual(b)[x] | 0x80;
		}
	    }
	    if (min_pos != INT_MAX) { // or max_pos != 0; ie not reset yet
		// not indel at this pos, but still in range.
		//*qual = 22 | 0x80;
		*qual = bam_get_qual(b)[plp[i].qpos] | 0x80;
	    }

#if 0
	    if (plp[i].indel || plp[i].is_del) {
//		mask_LC(b2, b, pos);
//		fprintf(stderr, "Pos %d, indel = %d\n", pos, plp[i].indel);
//		mask_LC(b2, b, pos+indel);
		// mask_indels(b2);

//		// Mark surrounding D bases
//		int x, x_s = plp[i].qpos+1 - INDEL_DIST, x_e = plp[i].qpos+1 + INDEL_DIST;
//		if (x_s < 0) x_s = 0;
//		if (x_e >= b->core.l_qseq) x_e = b->core.l_qseq-1;
//		for (x = x_s; x <= x_e; x++)
//		    bam_get_qual(b2)[x] = bam_get_qual(b)[x] | 0x80;
	    } else if (indel) {
//		mask_LC(b2, b, pos);
//		mask_LC(b2, b, pos+indel);

//
//		// Mark surrounding D/2 bases (or D?)
//		// FIXME: ideally neither; compute low complexity extents.
//		int x, x_s = plp[i].qpos+1 - INDEL_DIST/2, x_e = plp[i].qpos+1 + INDEL_DIST/2;
//		if (x_s < 0) x_s = 0;
//		if (x_e >= b->core.l_qseq) x_e = b->core.l_qseq-1;
//		for (x = x_s; x <= x_e; x++)
//		    bam_get_qual(b2)[x] = bam_get_qual(b)[x] | 0x80;
	    }
#endif

	    if (preserve)
		*qual |= 0x80;

	    if (!(*qual & 0x80)) {
		if (base == call1 || base == call2)
		    *qual = QH;
		else
		    //*qual = QL;
		    *qual = bin2[*qual];
	    }
	}

#ifdef DEBUG
	printf("\n");
#endif

	// Push any finished sequence to the b_hist list,
	// clearing the qual mask bit as we go.
	bi = bl->s_head;
	for (i = 0; i < n_plp; i++) {
	    bam1_t *b2 = bi->b;

	    uint8_t *qual = bam_get_qual(b2);

	    if (!plp[i].is_tail) {
		bi = bi->s_next;
		continue;
	    }

	    // Correct qualities
	    int x;
	    for (x = 0; x < b2->core.l_qseq; x++) {
		if (qual[x] & 0x80)
		    qual[x] &= ~0x80;
	    }

	    uint64_t id = bi->id;
	    bam_sorted_item *next = bi->s_next;
	    remove_bam_list(bl, bi);
	    bi = next;

	    // Note: this may reorder seqs that start at the same coord,
	    // so we give it the read-id to preserve the order.
	    insert_bam_list_id(b_hist, b2, id);
	}

	// Flush history (preserving sort order).
	flush_bam_list(b_hist, left_most, out, header);
    }

    flush_bam_list(b_hist, INT_MAX, out, header);

    bam_plp_destroy(p_iter);
    bam_hdr_destroy(header);
    if (h_iter) hts_itr_destroy(h_iter);

    if (sam_close(in) != 0) {
	fprintf(stderr, "Error while closing input fd\n");
	return 1;
    }

    if (out && sam_close(out) != 0) {
	fprintf(stderr, "Error while closing output fd\n");
	return 1;
    }

    // free sorted list

    return 0;
}


/*

Mapping quality distribution on first 2Gb of 16250_1.bam

0 1214051
1 29715
2 12905
3 41297
4 13323
5 13415
6 55322
7 16876
8 10478
9 19654
10 7667
11 6675
12 22447
13 8630
14 5123
15 12154
16 6180
17 6074
18 11340
19 7864
20 9194
21 12132
22 14394
23 10927
24 14694
25 23238
26 3576
27 71092
28 3932
29 3708
30 8752
31 5505
32 4633
33 7899
34 4227
35 3813
36 6539
37 5517
38 3208
39 10909
40 161991
41 9579
42 8908
43 14373
44 10640
45 18752
46 19069
47 26576
48 62546
49 5956
50 9120
51 7198
52 11103
53 5500
54 10075
55 8144
56 6276
57 24364
58 5976
59 12047
60 25222728

27410000 reads, so mqual 0 is 4.4% of the data.

*/