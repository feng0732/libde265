/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Bitstream-semantics tests for short-term reference picture sets
 * (read_short_term_ref_pic_set, H.265 clause 7.3.7 / 7.4.8).
 *
 * The expected DeltaPocS0/DeltaPocS1 lists and UsedByCurrPic flags are taken
 * directly from the RPS syntax/semantics of the standard, independently of how
 * the rest of the decoder happens to order its internal arrays.
 */

#include "refpic_test_harness.h"

using namespace refpic_test;

// Defined in libde265/refpic.cc; not part of refpic.h's public surface.
extern bool read_short_term_ref_pic_set(error_queue* errqueue,
                                        const seq_parameter_set* sps,
                                        bitreader* br,
                                        ref_pic_set* out_set,
                                        uint32_t idxRps,
                                        const std::vector<ref_pic_set>& sets,
                                        bool sliceRefPicSet);

namespace {

// Parse one explicitly coded RPS from the given bytes.
ref_pic_set parse_explicit_rps(error_queue* errqueue,
                               const seq_parameter_set* sps,
                               const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> mutable_bytes = bytes;
  bitreader br(mutable_bytes.data(), static_cast<int>(mutable_bytes.size()));

  std::vector<ref_pic_set> sets;
  ref_pic_set out;
  out.reset();

  const bool ok = read_short_term_ref_pic_set(errqueue, sps, &br, &out,
                                              /*idxRps=*/0, sets,
                                              /*sliceRefPicSet=*/false);
  EXPECT_TRUE(ok);
  return out;
}

// Build the previously decoded sets vector needed for predicted RPS parsing.
// Each entry is an already-derived ref_pic_set.
std::vector<ref_pic_set> make_prior_sets(std::initializer_list<ref_pic_set> sets) {
  return std::vector<ref_pic_set>(sets);
}

ref_pic_set make_set(std::initializer_list<StEntry> entries) {
  ref_pic_set s;
  s.reset();
  for (const StEntry& e : entries) {
    if (e.delta_poc < 0) {
      const int i = s.NumNegativePics++;
      s.DeltaPocS0[i] = static_cast<int16_t>(e.delta_poc);
      s.UsedByCurrPicS0[i] = e.used_by_curr_pic ? 1 : 0;
    } else if (e.delta_poc > 0) {
      const int i = s.NumPositivePics++;
      s.DeltaPocS1[i] = static_cast<int16_t>(e.delta_poc);
      s.UsedByCurrPicS1[i] = e.used_by_curr_pic ? 1 : 0;
    }
  }
  s.compute_derived_values();
  return s;
}

// Encode a predicted RPS (inter_ref_pic_set_prediction_flag == 1).
// 'flags' is the per-entry (nDeltaPocsRIdx+1) array of
// (used_by_curr_pic_flag, use_delta_flag) pairs, in the order required by the
// syntax: S0 entries, then S1 entries, then the delta-RPS-zero entry.
struct PredFlag {
  bool used_by_curr_pic;
  bool use_delta;  // ignored when used_by_curr_pic is true (implied 1)
};

std::vector<uint8_t> encode_predicted_st_rps(int delta_idx_minus1,
                                             int delta_rps_sign,
                                             uint32_t abs_delta_rps_minus1,
                                             const std::vector<PredFlag>& flags) {
  BitWriter w;
  w.write_bit(1);  // inter_ref_pic_set_prediction_flag
  w.write_uvlc(static_cast<uint32_t>(delta_idx_minus1));
  w.write_bit(delta_rps_sign);
  w.write_uvlc(abs_delta_rps_minus1);
  for (const PredFlag& f : flags) {
    w.write_bit(f.used_by_curr_pic ? 1 : 0);
    if (!f.used_by_curr_pic) {
      w.write_bit(f.use_delta ? 1 : 0);
    }
  }
  w.write_rbsp_trailing_bits();
  return w.bytes();
}

}  // namespace

// Explicit RPS with pictures on both sides of the current picture: the S0 list
// must be sorted by decreasing POC and the S1 list by increasing POC.
TEST(ShortTermRefPicSetTest, ExplicitSetKeepsSortedDeltaPocLists) {
  // Decode order 0,3,1,2 (classic low-delay-free B structure): when picture
  // POC=3 is decoded first after POC=0, its RPS references -3 (POC 0) and the
  // later pictures -2 (POC 1) and -1 (POC 2) as "following" entries.
  const std::vector<StEntry> entries = {
      {-3, true}, {-2, false}, {-1, true}, {+1, false},
  };

  error_queue errqueue;
  seq_parameter_set sps;
  sps.set_defaults();
  sps.sps_max_sub_layers = 1;
  sps.sps_max_dec_pic_buffering[0] = 8;

  ref_pic_set set = parse_explicit_rps(&errqueue, &sps,
                                       encode_explicit_st_rps(entries));

  ASSERT_EQ(set.NumNegativePics, 3);
  ASSERT_EQ(set.NumPositivePics, 1);
  EXPECT_EQ(set.NumDeltaPocs, 4);

  // S0: decreasing order (closest negative delta first).
  EXPECT_EQ(set.DeltaPocS0[0], -1);
  EXPECT_EQ(set.DeltaPocS0[1], -2);
  EXPECT_EQ(set.DeltaPocS0[2], -3);

  // used_by_curr_pic flags survive the round trip.
  EXPECT_EQ(set.UsedByCurrPicS0[0], 1);  // -1 used
  EXPECT_EQ(set.UsedByCurrPicS0[1], 0);  // -2 only kept for later pictures
  EXPECT_EQ(set.UsedByCurrPicS0[2], 1);  // -3 used

  // S1: increasing order.
  EXPECT_EQ(set.DeltaPocS1[0], +1);
  EXPECT_EQ(set.UsedByCurrPicS1[0], 0);

  // Only the entries flagged used_by_curr_pic count as current references.
  EXPECT_EQ(set.NumPocTotalCurr_shortterm_only, 2);
}

// An explicitly coded set containing only future (positive POC) pictures must
// leave the negative list empty instead of misplacing entries.
TEST(ShortTermRefPicSetTest, ExplicitSetWithOnlyFuturePictures) {
  const std::vector<StEntry> entries = {{+1, true}, {+2, true}, {+4, false}};

  error_queue errqueue;
  seq_parameter_set sps;
  sps.set_defaults();
  sps.sps_max_sub_layers = 1;
  sps.sps_max_dec_pic_buffering[0] = 8;

  ref_pic_set set = parse_explicit_rps(&errqueue, &sps,
                                       encode_explicit_st_rps(entries));

  EXPECT_EQ(set.NumNegativePics, 0);
  ASSERT_EQ(set.NumPositivePics, 3);
  EXPECT_EQ(set.DeltaPocS1[0], 1);
  EXPECT_EQ(set.DeltaPocS1[1], 2);
  EXPECT_EQ(set.DeltaPocS1[2], 4);
  EXPECT_EQ(set.UsedByCurrPicS1[0], 1);
  EXPECT_EQ(set.UsedByCurrPicS1[1], 1);
  EXPECT_EQ(set.UsedByCurrPicS1[2], 0);
  EXPECT_EQ(set.NumPocTotalCurr_shortterm_only, 2);
}

// Predicted RPS: shifting an existing set by a negative DeltaRPS moves every
// retained entry and adds the source current picture (delta 0 -> DeltaRPS),
// exactly as clause 7.4.8 derives the target lists.
TEST(ShortTermRefPicSetTest, PredictedSetWithNegativeDeltaRps) {
  // Source set (RIdx): references POC-1, keeps POC-2 for later and POC+1 for
  // the current picture.
  const ref_pic_set source = make_set({{-1, true}, {-2, false}, {+1, true}});
  std::vector<ref_pic_set> prior = make_prior_sets({source});

  // Target set predicted from set 0 with DeltaRPS = -1. The clause-7.4.8
  // derivation produces three retained negative entries (the shifted +1
  // lands on zero and is dropped by the dPoc<0 filter):
  //   source  0 + DeltaRPS = -1 -> S0 as -1 (used, shifted current picture)
  //   source -1 + DeltaRPS = -2 -> S0 as -2 (used)
  //   source -2 + DeltaRPS = -3 -> S0 as -3 (kept, not used by current)
  // Flag order per syntax: S0 entries (-1, -2), S1 entry (+1), then zero.
  const std::vector<PredFlag> flags = {
      {true, /*ignored*/ true},   // -1 -> -2, used
      {false, true},              // -2 -> -3, kept but not used by current
      {true, /*ignored*/ true},   // +1 ->  0, excluded by the strict-negative
                                  //         S0 filter
      {true, /*ignored*/ true},   // current -> -1, used
  };
  const std::vector<uint8_t> bytes =
      encode_predicted_st_rps(/*delta_idx_minus1=*/0, /*sign=*/1 /*negative*/,
                              /*abs_delta_rps_minus1=*/0, flags);

  std::vector<uint8_t> mutable_bytes = bytes;
  bitreader br(mutable_bytes.data(), static_cast<int>(mutable_bytes.size()));

  seq_parameter_set sps;
  sps.set_defaults();
  sps.sps_max_sub_layers = 1;
  sps.sps_max_dec_pic_buffering[0] = 8;

  ref_pic_set out;
  out.reset();
  error_queue errqueue;
  const bool ok = read_short_term_ref_pic_set(&errqueue, &sps, &br, &out,
                                              /*idxRps=*/1, prior,
                                              /*sliceRefPicSet=*/true);
  ASSERT_TRUE(ok);

  // All resulting deltas are negative and appear in decreasing POC order.
  ASSERT_EQ(out.NumNegativePics, 3);
  EXPECT_EQ(out.NumPositivePics, 0);
  const std::vector<int16_t> expected_deltas = {-1, -2, -3};
  const std::vector<uint8_t> expected_used = {1, 1, 0};
  for (size_t i = 0; i < expected_deltas.size(); ++i) {
    EXPECT_EQ(out.DeltaPocS0[i], expected_deltas[i]) << "delta index " << i;
    EXPECT_EQ(out.UsedByCurrPicS0[i], expected_used[i]) << "used index " << i;
  }
  EXPECT_EQ(out.NumPocTotalCurr_shortterm_only, 2);
}

// A picture dropped from a predicted RPS (used_by_curr_pic_flag == 0 and
// use_delta_flag == 0) must not appear in either output list.
TEST(ShortTermRefPicSetTest, PredictedSetDropsUnusedEntry) {
  const ref_pic_set source = make_set({{-1, true}, {-3, true}});
  std::vector<ref_pic_set> prior = make_prior_sets({source});

  // DeltaRPS = -2: -1 -> -3, -3 -> -5, current -> -2.
  // Keep -3 (used), drop the shifted -5 entirely, keep current as following.
  const std::vector<PredFlag> flags = {
      {true, true},    // -1 -> -3 used
      {false, false},  // -3 -> -5 dropped from the RPS
      {false, true},   // current -> -2 kept, not used by current pic
  };
  const std::vector<uint8_t> bytes =
      encode_predicted_st_rps(0, 1, /*abs_delta_rps_minus1=*/1, flags);

  std::vector<uint8_t> mutable_bytes = bytes;
  bitreader br(mutable_bytes.data(), static_cast<int>(mutable_bytes.size()));

  seq_parameter_set sps;
  sps.set_defaults();
  sps.sps_max_sub_layers = 1;
  sps.sps_max_dec_pic_buffering[0] = 8;

  ref_pic_set out;
  out.reset();
  error_queue errqueue;
  const bool ok = read_short_term_ref_pic_set(&errqueue, &sps, &br, &out,
                                              1, prior, true);
  ASSERT_TRUE(ok);

  ASSERT_EQ(out.NumNegativePics, 2);
  EXPECT_EQ(out.DeltaPocS0[0], -2);  // shifted current, following only
  EXPECT_EQ(out.UsedByCurrPicS0[0], 0);
  EXPECT_EQ(out.DeltaPocS0[1], -3);  // retained used reference
  EXPECT_EQ(out.UsedByCurrPicS0[1], 1);
  EXPECT_EQ(out.NumPositivePics, 0);
  EXPECT_EQ(out.NumPocTotalCurr_shortterm_only, 1);
}

// RPS capacity check: more entries than sps_max_dec_pic_buffering permits must
// be rejected rather than overflowing internal fixed-size arrays.
TEST(ShortTermRefPicSetTest, RejectsSetExceedingDpbCapacity) {
  std::vector<StEntry> entries;
  for (int i = 1; i <= 6; ++i) {
    entries.push_back({-i, true});
  }

  error_queue errqueue;
  seq_parameter_set sps;
  sps.set_defaults();
  sps.sps_max_sub_layers = 1;
  sps.sps_max_dec_pic_buffering[0] = 4;  // smaller than the coded RPS

  const std::vector<uint8_t> bytes = encode_explicit_st_rps(entries);
  std::vector<uint8_t> mutable_bytes = bytes;
  bitreader br(mutable_bytes.data(), static_cast<int>(mutable_bytes.size()));

  std::vector<ref_pic_set> prior;
  ref_pic_set out;
  out.reset();
  const bool ok = read_short_term_ref_pic_set(&errqueue, &sps, &br, &out,
                                              0, prior, false);
  EXPECT_FALSE(ok);
  EXPECT_EQ(out.NumDeltaPocs, 0);
}
