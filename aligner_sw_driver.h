/*
 * Copyright 2011, Ben Langmead <blangmea@jhsph.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *  aligner_sw_driver.h
 *
 * REDUNDANT SEED HITS
 *
 * We say that two seed hits are redundant if they trigger identical
 * seed-extend dynamic programming problems.  Put another way, they both lie on
 * the same diagonal of the overall read/reference dynamic programming matrix.
 * Detecting redundant seed hits is simple when the seed hits are ungapped.  We
 * do this after offset resolution but before the offset is converted to genome
 * coordinates (see uses of the seenDiags1_/seenDiags2_ fields for examples).
 *
 * REDUNDANT ALIGNMENTS
 *
 * In an unpaired context, we say that two alignments are redundant if they
 * share any cells in the global DP table.  Roughly speaking, this is like
 * saying that two alignments are redundant if any read character aligns to the
 * same reference character (same reference sequence, same strand, same offset)
 * in both alignments.
 *
 * In a paired-end context, we say that two paired-end alignments are redundant
 * if the mate #1s are redundant and the mate #2s are redundant.
 *
 * How do we enforce this?  In the unpaired context, this is relatively simple:
 * the cells from each alignment are checked against a set containing all cells
 * from all previous alignments.  Given a new alignment, for each cell in the
 * new alignment we check whether it is in the set.  If there is any overlap,
 * the new alignment is rejected as redundant.  Otherwise, the new alignment is
 * accepted and its cells are added to the set.
 *
 * Enforcement in a paired context is a little trickier.  Consider the
 * following approaches:
 *
 * 1. Skip anchors that are redundant with any previous anchor or opposite
 *    alignment.  This is sufficient to ensure no two concordant alignments
 *    found are redundant.
 *
 * 2. Same as scheme 1, but with a "transitive closure" scheme for finding all
 *    concordant pairs in the vicinity of an anchor.  Consider the AB/AC
 *    scenario from the previous paragraph.  If B is the anchor alignment, we
 *    will find AB but not AC.  But under this scheme, once we find AB we then
 *    let B be a new anchor and immediately look for its opposites.  Likewise,
 *    if we find any opposite, we make them anchors and continue searching.  We
 *    don't stop searching until every opposite is used as an anchor.
 *
 * 3. Skip anchors that are redundant with any previous anchor alignment (but
 *    allow anchors that are redundant with previous opposite alignments).
 *    This isn't sufficient to avoid redundant concordant alignments.  To avoid
 *    redundant concordants, we need an additional procedure that checks each
 *    new concordant alignment one-by-one against a list of previous concordant
 *    alignments to see if it is redundant.
 *
 * We take approach 1.
 */

#ifndef ALIGNER_SW_DRIVER_H_
#define ALIGNER_SW_DRIVER_H_

#include <utility>
#include "ds.h"
#include "aligner_seed.h"
#include "aligner_sw.h"
#include "aligner_cache.h"
#include "reference.h"
#include "group_walk.h"
#include "bt2_idx.h"
#include "mem_ids.h"
#include "aln_sink.h"
#include "pe.h"
#include "sa_rescomb.h"
#include "ival_list.h"
#include "simple_func.h"
#include "random_util.h"

struct SeedPos {

	SeedPos() : fw(false), offidx(0), rdoff(0), seedlen(0) { }

	SeedPos(
		bool fw_,
		uint32_t offidx_,
		uint32_t rdoff_,
		uint32_t seedlen_)
	{
		init(fw_, offidx_, rdoff_, seedlen_);
	}
	
	void init(
		bool fw_,
		uint32_t offidx_,
		uint32_t rdoff_,
		uint32_t seedlen_)
	{
		fw      = fw_;
		offidx  = offidx_;
		rdoff   = rdoff_;
		seedlen = seedlen_;
	}
	
	bool operator<(const SeedPos& o) const {
		if(offidx < o.offidx)   return true;
		if(offidx > o.offidx)   return false;
		if(rdoff < o.rdoff)     return true;
		if(rdoff > o.rdoff)     return false;
		if(seedlen < o.seedlen) return true;
		if(seedlen > o.seedlen) return false;
		if(fw && !o.fw)         return true;
		if(!fw && o.fw)         return false;
		return false;
	}
	
	bool operator>(const SeedPos& o) const {
		if(offidx < o.offidx)   return false;
		if(offidx > o.offidx)   return true;
		if(rdoff < o.rdoff)     return false;
		if(rdoff > o.rdoff)     return true;
		if(seedlen < o.seedlen) return false;
		if(seedlen > o.seedlen) return true;
		if(fw && !o.fw)         return false;
		if(!fw && o.fw)         return true;
		return false;
	}
	
	bool operator==(const SeedPos& o) const {
		return fw == o.fw && offidx == o.offidx &&
		       rdoff == o.rdoff && seedlen == o.seedlen;
	}

	bool fw;
	uint32_t offidx;
	uint32_t rdoff;
	uint32_t seedlen;
};

/**
 * An SATuple along with the associated seed position.
 */
struct SATupleAndPos {
	
	SATuple sat;    // result for this seed hit
	SeedPos pos;    // seed position that yielded the range this was taken from
	size_t  origSz; // size of range this was taken from
	
	bool operator<(const SATupleAndPos& o) const {
		if(sat < o.sat) return true;
		if(sat > o.sat) return false;
		return pos < o.pos;
	}

	bool operator==(const SATupleAndPos& o) const {
		return sat == o.sat && pos == o.pos;
	}
};

/**
 * Encapsulates the weighted random sampling scheme we want to use to pick
 * which seed hit range to sample a row from.
 */
class RowSampler {

public:

	RowSampler(int cat = 0) : elim_(cat), masses_(cat) { 
		mass_ = 0.0f;
	}
	
	/**
	 * Initialze sampler with respect to a range of elements in a list of
	 * SATupleAndPos's.
	 */
	void init(const EList<SATupleAndPos, 16>& salist, size_t sai, size_t saf) {
		assert_gt(saf, sai);
		elim_.resize(saf - sai);
		elim_.fill(false);
		// Initialize mass
		mass_ = 0.0f;
		masses_.resize(saf - sai);
		for(size_t i = sai; i < saf; i++) {
			masses_[i - sai] = (1.0f / sqrt((double)salist[i].sat.size()));
			mass_ += masses_[i - sai];
		}
	}
	
	/**
	 * Caller is indicating that the bin at index i is exhausted and we should
	 * exclude it from our sampling from now on.
	 */
	void finishedRange(size_t i) {
		assert_lt(i, masses_.size());
		elim_[i] = true;
		mass_ -= masses_[i];
	}
	
	/**
	 * Sample randomly from the mass.
	 */
	size_t next(RandomSource& rnd) {
		// Throw the dart
		double rd = rnd.nextFloat() * mass_;
		double mass_sofar = 0.0f;
		for(size_t i = 0; i < masses_.size(); i++) {
			if(!elim_[i]) {
				mass_sofar += masses_[i];
				if(rd < mass_sofar) {
					// This is the one we hit
					return i;
				}
			}
		}
		assert(false);
		return 0;
	}

protected:
	double        mass_;    // total probability mass to throw darts at
	EList<bool>   elim_;    // whether the range is eliminated
	EList<double> masses_;  // mass of each range
};

class SwDriver {

public:

	SwDriver() :
		satups_(DP_CAT),
		sacomb_(DP_CAT),
		gws_(DP_CAT),
		seenDiags1_(DP_CAT),
		seenDiags2_(DP_CAT),
		redAnchor_(DP_CAT),
		redMate1_(DP_CAT),
		redMate2_(DP_CAT),
		//st_(1024, DP_CAT),
		en_(1024, DP_CAT),
		res_(), ores_()
	{ }

	/**
	 * Given a collection of SeedHits for a single read, extend seed alignments
	 * into full alignments.  Where possible, try to avoid redundant offset
	 * lookups and dynamic programming problems.  Optionally report alignments
	 * to a AlnSinkWrap object as they are discovered.
	 *
	 * If 'reportImmediately' is true, returns true iff a call to
	 * mhs->report() returned true (indicating that the reporting
	 * policy is satisfied and we can stop).  Otherwise, returns false.
	 */
	bool extendSeeds(
		const Read& rd,              // read to align
		bool mate1,                  // true iff rd is mate #1
		bool color,                  // true -> read is colorspace
		SeedResults& sh,             // seed hits to extend into full alignments
		const Ebwt& ebwt,            // BWT
		const BitPairReference& ref, // Reference strings
		SwAligner& swa,              // dynamic programming aligner
		const Scoring& sc,           // scoring scheme
		int seedmms,                 // # mismatches allowed in seed
		int seedlen,                 // length of seed
		int seedival,                // interval between seeds
		TAlScore minsc,              // minimum score for anchor
		TAlScore floorsc,            // local-alignment floor for anchor score
		int nceil,                   // maximum # Ns permitted in ref portion
		const SimpleFunc& maxelt,    // # elts to explore as function of total elts
		size_t maxhalf,              // maximum width on one side of DP table
		bool enable8,                // use 8-bit SSE where possible
		bool refscan,                // use reference scanning
		int tighten,                 // -M score tightening mode
		AlignmentCacheIface& ca,     // alignment cache for seed hits
		RandomSource& rnd,           // pseudo-random source
		WalkMetrics& wlm,            // group walk left metrics
		SwMetrics& swmSeed,          // DP metrics for seed-extend
		AlnSinkWrap* mhs,            // HitSink for multiseed-style aligner
		bool reportImmediately,      // whether to report hits immediately to mhs
		EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
		EList<SwActionSink*>* swActionSinks,   // send action-list updates to these
		bool& exhaustive);

	/**
	 * Given a read, perform full dynamic programming against the entire
	 * reference.  Optionally report alignments to a AlnSinkWrap object
	 * as they are discovered.
	 *
	 * If 'reportImmediately' is true, returns true iff a call to
	 * mhs->report() returned true (indicating that the reporting
	 * policy is satisfied and we can stop).  Otherwise, returns false.
	 */
	bool sw(
		const Read& rd,              // read to align
		bool color,                  // true -> read is colorspace
		const BitPairReference& ref, // Reference strings
		SwAligner& swa,              // dynamic programming aligner
		const Scoring& sc,           // scoring scheme
		TAlScore minsc,              // minimum score for anchor
		TAlScore floorsc,            // local-alignment floor for anchor score
		RandomSource& rnd,           // pseudo-random source
		SwMetrics& swm,              // dynamic programming metrics
		AlnSinkWrap* mhs,            // HitSink for multiseed-style aligner
		bool reportImmediately,      // whether to report hits immediately to mhs
		EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
		EList<SwActionSink*>* swActionSinks);  // send action-list updates to these

	/**
	 * Given a collection of SeedHits for a read pair, extend seed
	 * alignments into full alignments and then look for the opposite
	 * mate using dynamic programming.  Where possible, try to avoid
	 * redundant offset lookups.  Optionally report alignments to a
	 * AlnSinkWrap object as they are discovered.
	 *
	 * If 'reportImmediately' is true, returns true iff a call to
	 * mhs->report() returned true (indicating that the reporting
	 * policy is satisfied and we can stop).  Otherwise, returns false.
	 */
	bool extendSeedsPaired(
		const Read& rd,              // mate to align as anchor
		const Read& ord,             // mate to align as opposite
		bool anchor1,                // true iff anchor mate is mate1
		bool oppFilt,                // true iff opposite mate was filtered out
		bool color,                  // true -> reads are colorspace
		SeedResults& sh,             // seed hits for anchor
		const Ebwt& ebwt,            // BWT
		const BitPairReference& ref, // Reference strings
		SwAligner& swa,              // dyn programming aligner for anchor
		SwAligner& swao,             // dyn programming aligner for opposite
		const Scoring& sc,           // scoring scheme
		const PairedEndPolicy& pepol,// paired-end policy
		int seedmms,                 // # mismatches allowed in seed
		int seedlen,                 // length of seed
		int seedival,                // interval between seeds
		TAlScore minsc,              // minimum score for anchor
		TAlScore ominsc,             // minimum score for opposite
		TAlScore floorsc,            // local-alignment floor for anchor score
		TAlScore ofloorsc,           // local-alignment floor for opposite score
		int nceil,                   // max # Ns permitted in ref for anchor
		int onceil,                  // max # Ns permitted in ref for opposite
		bool nofw,                   // don't align forward read
		bool norc,                   // don't align revcomp read
		const SimpleFunc& maxelt,    // # elts to explore as function of total elts
		size_t maxhalf,              // maximum width on one side of DP table
		bool enable8,                // use 8-bit SSE where possible
		bool refscan,                // use reference scanning
		int tighten,                 // -M score tightening mode
		AlignmentCacheIface& cs,     // alignment cache for seed hits
		RandomSource& rnd,           // pseudo-random source
		WalkMetrics& wlm,            // group walk left metrics
		SwMetrics& swmSeed,          // DP metrics for seed-extend
		SwMetrics& swmMate,          // DP metrics for mate finidng
		AlnSinkWrap* msink,          // AlnSink wrapper for multiseed-style aligner
		bool swMateImmediately,      // whether to look for mate immediately
		bool reportImmediately,      // whether to report hits immediately to msink
		bool discord,                // look for discordant alignments?
		bool mixed,                  // look for unpaired as well as paired alns?
		EList<SwCounterSink*>* swCounterSinks, // send counter updates to these
		EList<SwActionSink*>* swActionSinks,   // send action-list updates to these
		bool& exhaustive);

	/**
	 * Prepare for a new read.  For paired-end reads, this means clearing state
	 * that would otherwise survive across calls to extendSeedsPaired.
	 */
	void nextRead(bool paired, size_t mate1len, size_t mate2len) {
		redAnchor_.reset();
		seenDiags1_.reset();
		seenDiags2_.reset();
		size_t maxlen = mate1len;
		if(paired) {
			redMate1_.reset();
			redMate1_.init(mate1len);
			redMate2_.reset();
			redMate2_.init(mate2len);
			if(mate2len > maxlen) {
				maxlen = mate2len;
			}
		}
		redAnchor_.init(maxlen);
	}

protected:

	/**
	 * Given seed results, set up all of our state for resolving and keeping
	 * track of reference offsets for hits.
	 */
#if 0
	void setUpSaRangeState(
		SeedResults& sh,             // seed hits to extend into full alignments
		const Ebwt& ebwt,            // BWT
		const BitPairReference& ref, // Reference strings
		int seedlen,                 // length of seed
		const SimpleFunc& rowmult,   // max rows to consider per position
		bool scanNarrowed,           // true -> ref scan even for narrowed hits
		AlignmentCacheIface& ca,     // alignment cache for seed hits
		RandomSource& rnd,           // pseudo-random generator
		WalkMetrics& wlm);           // group walk left metrics
#endif

	void resolveAll(
		SeedResults& sh,             // seed hits to extend into full alignments
		const Ebwt& ebwt,            // BWT
		const BitPairReference& ref, // Reference strings
		AlignmentCacheIface& ca,     // alignment cache for seed hits
		RandomSource& rnd,           // pseudo-random generator
		WalkMetrics& wlm,            // group walk left metrics
		size_t& nelt_out);           // out: # elements total

	void prioritizeSATups(
		SeedResults& sh,             // seed hits to extend into full alignments
		const Ebwt& ebwt,            // BWT
		const BitPairReference& ref, // Reference strings
		bool refscan,                // do reference scanning?
		size_t maxelt,               // max elts we'll consider
		size_t nsm,                  // if range as <= nsm elts, it's "small"
		AlignmentCacheIface& ca,     // alignment cache for seed hits
		RandomSource& rnd,           // pseudo-random generator
		WalkMetrics& wlm,            // group walk left metrics
		size_t& nelt);               // out: # elements total

	Random1toN               rand_;    // random number generators
	EList<Random1toN, 16>    rands_;   // random number generators
	EList<Random1toN, 16>    rands2_;  // random number generators
	EList<SATupleAndPos, 16> satpos_;  // holds SATuple, SeedPos pairs
	EList<SATupleAndPos, 16> satpos2_; // holds SATuple, SeedPos pairs
	EList<SATuple, 16>       satups_;  // holds SATuples to explore elements from
	EList<SAResolveCombiner, 16> sacomb_; // temporary holder for combiners
	EList<GroupWalk2>        gws_;         // list of GroupWalks; no particular order
	RowSampler               rowsamp_;     // row sampler

	SeedScanner    sscan_;      // reference scanner for resolving seed hits
	SeedScanTable  sstab_;      // table of seeds to search for

	// Data structures encapsulating the diagonals that have already been used
	// to seed alignment for mate 1 and mate 2.
	EIvalMergeList seenDiags1_;
	EIvalMergeList seenDiags2_;

	// For weeding out redundant alignments
	RedundantAlns  redAnchor_;  // database of cells used for anchor alignments
	RedundantAlns  redMate1_;   // database of cells used for mate 1 alignments
	RedundantAlns  redMate2_;   // database of cells used for mate 2 alignments

	// For specifying starting and ending columns
	//EList<bool>    st_;         // temp holder for dyn prog starting mask
	EList<bool>    en_;         // temp holder for dyn prog ending mask
	//EList<bool>    ost_;        // like st_ but for opposite mate
	EList<bool>    oen_;        // like en_ but for opposite mate
	
	// For holding results for anchor (res_) and opposite (ores_) mates
	SwResult       res_;        // temp holder for SW results
	SwResult       ores_;       // temp holder for SW results for opp mate
	
	// For AlnRes::matchesRef:
	ASSERT_ONLY(SStringExpandable<char>     raw_refbuf_);
	ASSERT_ONLY(SStringExpandable<uint32_t> raw_destU32_);
	ASSERT_ONLY(EList<bool>                 raw_matches_);
	ASSERT_ONLY(BTDnaString                 tmp_rf_);
	ASSERT_ONLY(BTDnaString                 tmp_rdseq_);
	ASSERT_ONLY(BTString                    tmp_qseq_);
};

#endif /*ALIGNER_SW_DRIVER_H_*/
