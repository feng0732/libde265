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
 * Test harness for the HEVC reference picture management (H.265 clause 8.3).
 *
 * The expected values in every test are derived from the bitstream semantics
 * of H.265 (DPB marking rules, short-term RPS construction in clause 7.4.8 and
 * reference picture list construction in clause 8.5.3), not from the current
 * output of the library.
 *
 * The production headers expose the reference-management state of
 * decoder_context / decoded_picture_buffer through private sections. The tests
 * are white-box by necessity, so 'private' is remapped to 'public' before any
 * project header is included. This translation unit is test-only code; no
 * production header is modified.
 */

#ifndef LIBDE265_TESTS_REFPIC_TEST_HARNESS_H
#define LIBDE265_TESTS_REFPIC_TEST_HARNESS_H

// Must precede every project header: expose private DPB / decoder state so the
// tests can assert on the clause-8.3 marking results directly.
#define private public
#define protected public
#include "libde265/decctx.h"
#include "libde265/dpb.h"
#include "libde265/refpic.h"
#include "libde265/slice.h"
#include "libde265/sps.h"
#include "libde265/pps.h"
#include "libde265/image.h"
#include "libde265/bitstream.h"
#undef private
#undef protected

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace refpic_test {

// Smallest picture geometry used by the synthetic streams. The exact size is
// irrelevant for reference management; it only has to be allocatable.
constexpr int kPicWidth  = 64;
constexpr int kPicHeight = 64;

/*
 * Minimal MSB-first bit writer. Produces the same byte layout that
 * bitreader consumes (the VLC syntax of an short-term reference picture set
 * contains no emulation-prevention sensitive start-code handling, so a plain
 * writer is sufficient).
 */
class BitWriter {
public:
  void write_bits(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      const int bit = (value >> i) & 1u;
      if (byte_pos_ >= static_cast<int>(data_.size())) {
        data_.push_back(0);
      }
      data_[byte_pos_] |= static_cast<uint8_t>(bit << (7 - bit_pos_));
      if (++bit_pos_ == 8) {
        bit_pos_ = 0;
        ++byte_pos_;
      }
    }
  }

  // unsigned exp-Golomb, codeNum = value (ue(v), clause 9.2)
  void write_uvlc(uint32_t value) {
    // Map codeNum to the order of the code: codeNum in [2^m-1, 2^(m+1)-2).
    uint32_t code_num = value;
    int m = 0;
    uint32_t base = 0;   // 2^m - 1
    uint32_t range = 1;  // 2^m
    while (code_num >= base + range) {
      base += range;
      range <<= 1;
      ++m;
    }
    // m leading zero bits, then a 1 bit, then the m-bit suffix.
    for (int i = 0; i < m; ++i) {
      write_bits(0, 1);
    }
    write_bits(1, 1);
    write_bits(code_num - base, m);
  }

  void write_bit(int bit) { write_bits(bit ? 1u : 0u, 1); }

  // RBSP trailing bits (clause 7.3.2.1): one 1-bit, aligned with zero bits.
  void write_rbsp_trailing_bits() {
    write_bit(1);
    while (bit_pos_ != 0) {
      write_bit(0);
    }
  }

  const std::vector<uint8_t>& bytes() const { return data_; }

private:
  std::vector<uint8_t> data_;
  int byte_pos_ = 0;
  int bit_pos_ = 0;
};

// Description of one short-term RPS entry as coded in the bitstream.
struct StEntry {
  int delta_poc;   // relative POC, negative -> S0 list, positive -> S1 list
  bool used_by_curr_pic;
};

// Encode an explicitly signalled (non-predicted) short-term RPS, i.e. the
// syntax of clause 7.3.7 with inter_ref_pic_set_prediction_flag == 0.
// Entries may be supplied in any order; they are sorted into the syntax order
// (S0: decreasing POC, S1: increasing POC) before coding.
inline std::vector<uint8_t> encode_explicit_st_rps(std::vector<StEntry> entries) {
  std::vector<StEntry> negative;
  std::vector<StEntry> positive;
  for (const StEntry& e : entries) {
    if (e.delta_poc < 0) {
      negative.push_back(e);
    } else if (e.delta_poc > 0) {
      positive.push_back(e);
    }
  }
  std::sort(negative.begin(), negative.end(),
            [](const StEntry& a, const StEntry& b) {
              return a.delta_poc > b.delta_poc;  // closest to zero first
            });
  std::sort(positive.begin(), positive.end(),
            [](const StEntry& a, const StEntry& b) {
              return a.delta_poc < b.delta_poc;
            });

  BitWriter w;
  w.write_uvlc(static_cast<uint32_t>(negative.size()));
  w.write_uvlc(static_cast<uint32_t>(positive.size()));

  // S0: decreasing POC order, coded as cumulative positive delta values.
  int last_poc = 0;
  for (const StEntry& e : negative) {
    const int delta = last_poc - e.delta_poc;  // >= 1
    w.write_uvlc(static_cast<uint32_t>(delta - 1));
    w.write_bit(e.used_by_curr_pic ? 1 : 0);
    last_poc = e.delta_poc;
  }

  // S1: increasing POC order.
  last_poc = 0;
  for (const StEntry& e : positive) {
    const int delta = e.delta_poc - last_poc;  // >= 1
    w.write_uvlc(static_cast<uint32_t>(delta - 1));
    w.write_bit(e.used_by_curr_pic ? 1 : 0);
    last_poc = e.delta_poc;
  }

  w.write_rbsp_trailing_bits();
  return w.bytes();
}

/*
 * White-box fixture that owns a decoder_context together with a configured
 * SPS/PPS pair and provides helpers that drive the clause-8.3 reference
 * picture management machinery one picture at a time.
 */
class RefPicManagementTest : public ::testing::Test {
protected:
  // The POC modulus is selectable so wrap-around tests can use a small
  // MaxPicOrderCntLsb without allocating hundreds of pictures.
  explicit RefPicManagementTest(int log2_max_pic_order_cnt_lsb = 8)
      : log2_max_pic_order_cnt_lsb_(log2_max_pic_order_cnt_lsb) {}

  void SetUp() override {
    ctx_ = std::make_unique<decoder_context>();
    reset_long_term_arrays();

    auto sps = std::make_shared<seq_parameter_set>();
    sps->set_defaults();
    sps->sps_read = true;
    sps->seq_parameter_set_id = 0;
    sps->set_resolution(kPicWidth, kPicHeight);
    sps->sps_max_sub_layers = 1;
    sps->sps_max_dec_pic_buffering[0] = kDpbCapacity;
    sps->sps_max_num_reorder_pics[0] = kDpbCapacity;
    sps->log2_max_pic_order_cnt_lsb = log2_max_pic_order_cnt_lsb_;
    sps->MaxPicOrderCntLsb = 1 << sps->log2_max_pic_order_cnt_lsb;
    sps->BitDepth_Y = 8;
    sps->BitDepth_C = 8;
    sps->long_term_ref_pics_present_flag = true;
    // Populate every geometry/block-size derived field that de265_image
    // allocation and reference management read back from the SPS.
    ASSERT_EQ(sps->compute_derived_values(/*sanitize_values=*/true), DE265_OK);
    sps_ = sps;

    auto pps = std::make_shared<pic_parameter_set>();
    pps->set_defaults();
    pps->pps_read = true;
    pps->pic_parameter_set_id = 0;
    pps->seq_parameter_set_id = 0;
    pps->sps = sps;
    pps_ = pps;

    // Install the parameter sets where the decoder's internal lookups expect
    // them (private storage, exposed via the include remap above).
    ctx_->sps[0] = sps;
    ctx_->pps[0] = pps;
    ctx_->current_sps = sps;
    ctx_->current_pps = pps;

    ctx_->dpb.set_max_size_of_DPB(kDpbCapacity);
    ctx_->dpb.set_norm_size_of_DPB(kDpbCapacity);

    // Every synthetic picture is a normal trailing reference picture, so the
    // non-IDR / non-IRAP branch of process_reference_picture_set() is taken.
    ctx_->nal_unit_type = NAL_UNIT_TRAIL_R;
    ctx_->IdrPicFlag = false;
    ctx_->RapPicFlag = false;
  }

  void TearDown() override {
    ctx_.reset();
  }

  int max_pic_order_cnt_lsb() const { return 1 << log2_max_pic_order_cnt_lsb_; }

  // Clear the decoder-context long-term arrays so stale entries from an
  // earlier slice header can never leak into a subsequent picture.
  void reset_long_term_arrays() {
    for (int i = 0; i < MAX_NUM_REF_PICS; ++i) {
      ctx_->PocLsbLt[i] = 0;
      ctx_->UsedByCurrPicLt[i] = 0;
      ctx_->DeltaPocMsbCycleLt[i] = 0;
      ctx_->CurrDeltaPocMsbPresentFlag[i] = 0;
      ctx_->FollDeltaPocMsbPresentFlag[i] = 0;
    }
  }

  /*
   * Allocate a new picture in the DPB and make it the decoder's current
   * picture. Mirrors the part of process_slice_segment_header() that runs
   * before process_reference_picture_set():
   *  - the image starts in UsedForShortTermReference,
   *  - PicOrderCntVal / picture_order_cnt_lsb are assigned,
   *  - nal_hdr is populated so POC bookkeeping behaves like a TID0 picture.
   */
  de265_image* begin_picture(int poc) {
    const int idx = ctx_->dpb.new_image(sps_, ctx_.get(), 0, nullptr, true);
    if (idx < 0) {
      ADD_FAILURE() << "dpb.new_image failed for POC " << poc
                    << " with error " << -idx;
      return nullptr;
    }

    de265_image* image = ctx_->dpb.get_image(idx);
    image->set_headers(nullptr, sps_, pps_);
    image->decctx = ctx_.get();
    image->nal_hdr.nal_unit_type = NAL_UNIT_TRAIL_R;
    image->nal_hdr.nuh_temporal_id = 0;
    image->PicOrderCntVal = poc;
    image->picture_order_cnt_lsb = poc & (max_pic_order_cnt_lsb() - 1);
    image->PicState = UsedForShortTermReference;
    // Synthetic pictures are never held for display, so an evicted picture
    // (PicState -> UnusedForReference) is immediately reusable by the DPB.
    image->PicOutputFlag = false;
    image->integrity = INTEGRITY_CORRECT;

    ctx_->img = image;
    return image;
  }

  // Build a slice header whose short-term RPS contains exactly the given
  // delta-POC entries. Long-term lists stay empty unless overridden.
  std::unique_ptr<slice_segment_header>
  make_header_with_st_rps(const std::vector<StEntry>& entries) {
    auto hdr = std::make_unique<slice_segment_header>();
    hdr->reset();
    hdr->slice_pic_parameter_set_id = 0;
    hdr->first_slice_segment_in_pic_flag = true;
    hdr->slice_type = SLICE_TYPE_P;
    hdr->num_long_term_sps = 0;
    hdr->num_long_term_pics = 0;
    hdr->num_ref_idx_l0_active = 1;
    hdr->num_ref_idx_l1_active = 1;
    hdr->ref_pic_list_modification_flag_l0 = false;
    hdr->ref_pic_list_modification_flag_l1 = false;

    auto& rps = hdr->CurrRps;
    rps.reset();
    reset_long_term_arrays();
    for (const StEntry& e : entries) {
      if (e.delta_poc < 0) {
        const int i = rps.NumNegativePics++;
        rps.DeltaPocS0[i] = static_cast<int16_t>(e.delta_poc);
        rps.UsedByCurrPicS0[i] = e.used_by_curr_pic ? 1 : 0;
      } else if (e.delta_poc > 0) {
        const int i = rps.NumPositivePics++;
        rps.DeltaPocS1[i] = static_cast<int16_t>(e.delta_poc);
        rps.UsedByCurrPicS1[i] = e.used_by_curr_pic ? 1 : 0;
      }
    }
    rps.compute_derived_values();
    hdr->NumPocTotalCurr = rps.NumPocTotalCurr_shortterm_only;
    return hdr;
  }

  // Append a long-term reference entry (clause 7.3.6.3) to a slice header and
  // keep NumPocTotalCurr consistent with clause 7.4.7.1. The active LT lists
  // are stored on the decoder context (populated from the slice header during
  // real decoding), so they are mirrored here as well.
  void add_long_term_entry(slice_segment_header* hdr,
                           int poc_lsb_lt,
                           bool used_by_curr_pic,
                           bool delta_poc_msb_present,
                           uint32_t delta_poc_msb_cycle = 0) {
    const int i = hdr->num_long_term_sps + hdr->num_long_term_pics;
    EXPECT_LT(i, MAX_NUM_REF_PICS);
    hdr->poc_lsb_lt[i] = poc_lsb_lt;
    hdr->used_by_curr_pic_lt_flag[i] = used_by_curr_pic;
    hdr->delta_poc_msb_present_flag[i] = delta_poc_msb_present;
    hdr->delta_poc_msb_cycle_lt[i] = delta_poc_msb_cycle;
    hdr->num_long_term_pics = static_cast<uint8_t>(i + 1);

    // Mirror into the decoder-context arrays consumed by
    // process_reference_picture_set() (slice.cc normally fills these while
    // reading the slice header).
    ctx_->PocLsbLt[i] = poc_lsb_lt;
    ctx_->UsedByCurrPicLt[i] = used_by_curr_pic ? 1 : 0;
    ctx_->DeltaPocMsbCycleLt[i] = delta_poc_msb_cycle;

    if (used_by_curr_pic) {
      ++hdr->NumPocTotalCurr;
    }
  }

  de265_error run_reference_picture_set(slice_segment_header* hdr) {
    return ctx_->process_reference_picture_set(hdr);
  }

  bool run_reference_picture_lists(slice_segment_header* hdr) {
    return ctx_->construct_reference_picture_lists(hdr);
  }

  // Apply the eviction list produced by process_reference_picture_set(), the
  // same call the sequential/parallel slice decoders make before decoding.
  void apply_evictions(slice_segment_header* hdr) {
    ctx_->remove_images_from_dpb(hdr->RemoveReferencesList);
  }

  // --- DPB inspection helpers (expectations follow clause 8.3.2) ---

  const de265_image* find_picture_by_poc(int poc) const {
    for (size_t i = 0; i < ctx_->dpb.size(); ++i) {
      const de265_image* image = ctx_->dpb.get_image(static_cast<uint16_t>(i));
      if (image->PicOrderCntVal == poc) {
        return image;
      }
    }
    return nullptr;
  }

  size_t dpb_size() const { return ctx_->dpb.size(); }

  PictureState state_of_poc(int poc) const {
    const de265_image* image = find_picture_by_poc(poc);
    EXPECT_NE(image, nullptr);
    return image != nullptr ? image->PicState : UnusedForReference;
  }

  // POCs of the entries in the constructed RefPicList, in list order.
  std::vector<int> ref_pic_list_pocs(const slice_segment_header* hdr, int list) const {
    std::vector<int> pocs;
    const int n = list == 0 ? hdr->num_ref_idx_l0_active
                            : hdr->num_ref_idx_l1_active;
    for (int i = 0; i < n; ++i) {
      pocs.push_back(hdr->RefPicList_POC[list][i]);
    }
    return pocs;
  }

  std::vector<bool> ref_pic_list_long_term_flags(const slice_segment_header* hdr,
                                                 int list) const {
    std::vector<bool> flags;
    const int n = list == 0 ? hdr->num_ref_idx_l0_active
                            : hdr->num_ref_idx_l1_active;
    for (int i = 0; i < n; ++i) {
      flags.push_back(hdr->LongTermRefPic[list][i]);
    }
    return flags;
  }

  decoder_context* ctx() { return ctx_.get(); }
  const seq_parameter_set* sps() const { return sps_.get(); }

  /*
   * Populate the DPB with POCs [first_poc, first_poc+count), marking every
   * picture as a retained short-term reference so they all coexist. Each
   * picture's RPS references all earlier POCs, matching how a real
   * low-delay-free stream keeps its reference window alive.
   */
  void seed_retained_short_term_pictures(int first_poc, int count) {
    std::vector<StEntry> entries;
    for (int i = 0; i < count; ++i) {
      const int poc = first_poc + i;
      de265_image* p = begin_picture(poc);
      ASSERT_NE(p, nullptr);
      auto hdr = make_header_with_st_rps(entries);
      ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
      apply_evictions(hdr.get());
      // Relative to the next picture every delta shifts by one, and the
      // just-decoded picture becomes the closest negative entry.
      for (StEntry& e : entries) {
        --e.delta_poc;
      }
      entries.insert(entries.begin(), {-1, true});
    }
  }

  static constexpr uint8_t kDpbCapacity = 8;

private:
  int log2_max_pic_order_cnt_lsb_ = 8;  // MaxPicOrderCntLsb = 256 unless overridden
  std::unique_ptr<decoder_context> ctx_;
  std::shared_ptr<seq_parameter_set> sps_;
  std::shared_ptr<pic_parameter_set> pps_;
};

// Fixture variant with a small POC modulus (MaxPicOrderCntLsb == 16) so
// wrap-around can be exercised with a handful of pictures.
class RefPicManagementWrapTest : public RefPicManagementTest {
protected:
  RefPicManagementWrapTest() : RefPicManagementTest(/*log2_max_poc_lsb=*/4) {}
};

}  // namespace refpic_test

#endif  // LIBDE265_TESTS_REFPIC_TEST_HARNESS_H
