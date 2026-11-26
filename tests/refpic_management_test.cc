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
 * Reference picture management tests (H.265 clause 8.3 / 8.5.3).
 *
 * The scenarios cover:
 *   1. decode order differing from display order (B pictures, positive-delta
 *      short-term references already present in the DPB),
 *   2. picture-order-count wrap-around (short-term and LSB-addressed long-term
 *      references across the MaxPicOrderCntLsb boundary),
 *   3. short-term and long-term references coexisting in one RPS,
 *   4. eviction of pictures that are no longer referenceable and reference
 *      picture list modification.
 *
 * Every expected POC, list entry and marking state is derived from the
 * bitstream semantics of the standard rather than the library output.
 */

#include "refpic_test_harness.h"

using namespace refpic_test;

namespace {

// Fill the luma plane of a DPB picture with a recognizable value so that a
// reference list entry can be checked against the picture *content* it points
// to, not merely against an array index.
void tag_picture(de265_image* image, uint8_t value) {
  ASSERT_NE(image, nullptr);
  image->fill_image(value, 128, 128);
}

uint8_t luma_value_at(const de265_image* image, int x, int y) {
  return *image->get_image_plane_at_pos(0, x, y);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Decode order != display order
// ---------------------------------------------------------------------------

// Display order POC 0,1,2,3 is decoded in order 0,3,1,2. When POC 3 is decoded
// it cannot reference POC 1/2 yet (they do not exist), but when POC 1 is
// decoded it references the already-decoded POC 0 (past) and POC 3 (future).
// The clause-8.5.3.2 reference list ordering must still map every entry to the
// correct picture content and marking state.
TEST_F(RefPicManagementTest, BPictureReferencesPastAndFutureInDisplayOrder) {
  // Decode POC 0: no references, stays in the DPB as a short-term reference.
  de265_image* pic0 = begin_picture(0);
  ASSERT_NE(pic0, nullptr);
  tag_picture(pic0, 10);
  {
    auto hdr = make_header_with_st_rps({});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }
  EXPECT_EQ(state_of_poc(0), UsedForShortTermReference);

  // Decode POC 3 next (decode order 0,3,...): references only POC 0.
  de265_image* pic3 = begin_picture(3);
  ASSERT_NE(pic3, nullptr);
  tag_picture(pic3, 30);
  {
    auto hdr = make_header_with_st_rps({{-3, true}});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());

    // POC 0 must resolve to the DPB picture tagged 10.
    const de265_image* referenced = find_picture_by_poc(0);
    ASSERT_NE(referenced, nullptr);
    EXPECT_EQ(luma_value_at(referenced, 0, 0), 10);
    EXPECT_EQ(referenced->PicState, UsedForShortTermReference);
  }

  // Decode POC 1: references past POC 0 and future POC 3 simultaneously.
  de265_image* pic1 = begin_picture(1);
  ASSERT_NE(pic1, nullptr);
  tag_picture(pic1, 11);
  std::unique_ptr<slice_segment_header> hdr;
  {
    hdr = make_header_with_st_rps({{-1, true}, {+2, true}});
    hdr->slice_type = SLICE_TYPE_B;
    hdr->num_ref_idx_l0_active = 2;
    hdr->num_ref_idx_l1_active = 2;
    hdr->NumPocTotalCurr = 2;

    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());

    // Marking result (clause 8.3.2): both referenced pictures remain short
    // term; the current picture (POC 1) is short term as well.
    EXPECT_EQ(state_of_poc(0), UsedForShortTermReference);
    EXPECT_EQ(state_of_poc(3), UsedForShortTermReference);
    EXPECT_EQ(pic1->PicState, UsedForShortTermReference);

    ASSERT_TRUE(run_reference_picture_lists(hdr.get()));

    // RefPicList0: StCurrBefore (past, POC 0) first, then StCurrAfter
    // (future, POC 3) - clause 8.5.3.2.3.
    EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{0, 3}));
    // RefPicList1: StCurrAfter (future, POC 3) first, then StCurrBefore.
    EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 1), (std::vector<int>{3, 0}));

    // Every list entry still points at the content of the picture with that
    // POC.
    for (int list = 0; list < 2; ++list) {
      for (int i = 0; i < 2; ++i) {
        const int poc = hdr->RefPicList_POC[list][i];
        const de265_image* target =
            ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[list][i]));
        ASSERT_NE(target, nullptr);
        EXPECT_EQ(target->PicOrderCntVal, poc);
        EXPECT_EQ(target->PicState, UsedForShortTermReference);
        const uint8_t expected_tag = static_cast<uint8_t>(poc == 0 ? 10 : 30);
        EXPECT_EQ(luma_value_at(target, 0, 0), expected_tag);
        EXPECT_FALSE(hdr->LongTermRefPic[list][i]);
      }
    }
  }

  // Decode POC 2 last: references POC 1 (past) and POC 3 (future).
  de265_image* pic2 = begin_picture(2);
  ASSERT_NE(pic2, nullptr);
  tag_picture(pic2, 12);
  {
    auto hdr2 = make_header_with_st_rps({{-1, true}, {+1, true}});
    hdr2->slice_type = SLICE_TYPE_B;
    hdr2->num_ref_idx_l0_active = 2;
    hdr2->num_ref_idx_l1_active = 2;
    hdr2->NumPocTotalCurr = 2;

    ASSERT_EQ(run_reference_picture_set(hdr2.get()), DE265_OK);
    apply_evictions(hdr2.get());
    ASSERT_TRUE(run_reference_picture_lists(hdr2.get()));

    EXPECT_EQ(ref_pic_list_pocs(hdr2.get(), 0), (std::vector<int>{1, 3}));
    EXPECT_EQ(ref_pic_list_pocs(hdr2.get(), 1), (std::vector<int>{3, 1}));

    // Content identity is preserved despite the reordering.
    const de265_image* past =
        ctx()->get_image(static_cast<uint16_t>(hdr2->RefPicList[0][0]));
    const de265_image* future =
        ctx()->get_image(static_cast<uint16_t>(hdr2->RefPicList[0][1]));
    EXPECT_EQ(luma_value_at(past, 0, 0), 11);
    EXPECT_EQ(luma_value_at(future, 0, 0), 30);
  }
}

// A short-term RPS entry with used_by_curr_pic == 0 still keeps the picture in
// the DPB for a later picture (PocStFoll): it must not be evicted yet, but it
// must not appear in the current reference lists either.
TEST_F(RefPicManagementTest, FollowingShortTermPictureIsRetainedButNotReferenced) {
  de265_image* pic0 = begin_picture(0);
  ASSERT_NE(pic0, nullptr);
  {
    auto hdr = make_header_with_st_rps({});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }

  de265_image* pic1 = begin_picture(1);
  ASSERT_NE(pic1, nullptr);
  {
    auto hdr = make_header_with_st_rps({{-1, true}});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }

  // POC 2 keeps POC 0 only as a following picture (not used for prediction).
  de265_image* pic2 = begin_picture(2);
  ASSERT_NE(pic2, nullptr);
  auto hdr = make_header_with_st_rps({{-2, false}, {-1, true}});
  hdr->num_ref_idx_l0_active = 1;
  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());

  // POC 0 remains referenceable in the DPB (PocStFoll) ...
  EXPECT_EQ(state_of_poc(0), UsedForShortTermReference);
  // ... but the active L0 list contains only POC 1.
  ASSERT_TRUE(run_reference_picture_lists(hdr.get()));
  EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{1}));
}

// ---------------------------------------------------------------------------
// 2. Picture order count wrap-around
// ---------------------------------------------------------------------------

// With MaxPicOrderCntLsb == 16, decoding POC 14 then POC 16 wraps the LSB
// (14 -> 0). Short-term references on both sides of the wrap must resolve to
// the correct absolute-POC pictures instead of colliding on the same LSB.
TEST_F(RefPicManagementWrapTest, ShortTermReferencesAcrossPocWrap) {
  // POC 14 (LSB 14), then POC 15 (LSB 15), then POC 16 (LSB 0).
  de265_image* pic14 = begin_picture(14);
  ASSERT_NE(pic14, nullptr);
  tag_picture(pic14, 14);
  {
    auto hdr = make_header_with_st_rps({});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }

  de265_image* pic15 = begin_picture(15);
  ASSERT_NE(pic15, nullptr);
  tag_picture(pic15, 15);
  {
    auto hdr = make_header_with_st_rps({{-1, true}});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }

  // POC 16 has LSB 0; the absolute-POC lookup for -1 and -2 must find the
  // pre-wrap pictures POC 15 and POC 14, not a phantom LSB-0 picture.
  de265_image* pic16 = begin_picture(16);
  ASSERT_NE(pic16, nullptr);
  tag_picture(pic16, 16);
  auto hdr = make_header_with_st_rps({{-2, true}, {-1, true}});
  hdr->num_ref_idx_l0_active = 2;
  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());

  EXPECT_EQ(state_of_poc(14), UsedForShortTermReference);
  EXPECT_EQ(state_of_poc(15), UsedForShortTermReference);

  ASSERT_TRUE(run_reference_picture_lists(hdr.get()));
  EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{14, 15}));

  const de265_image* entry0 =
      ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[0][0]));
  const de265_image* entry1 =
      ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[0][1]));
  EXPECT_EQ(luma_value_at(entry0, 0, 0), 14);
  EXPECT_EQ(luma_value_at(entry1, 0, 0), 15);
}

// A long-term reference addressed by poc_lsb_lt across the wrap must be matched
// on the LSB and marked UsedForLongTermReference even though a newer picture
// now carries the same absolute POC space.
TEST_F(RefPicManagementWrapTest, LongTermReferenceMatchedByLsbAcrossWrap) {
  // POC 0 becomes a long-term reference. Its LSB is 0.
  de265_image* pic0 = begin_picture(0);
  ASSERT_NE(pic0, nullptr);
  tag_picture(pic0, 100);
  {
    auto hdr = make_header_with_st_rps({});
    add_long_term_entry(hdr.get(), /*poc_lsb_lt=*/0,
                        /*used_by_curr_pic=*/true,
                        /*delta_poc_msb_present=*/false);
    hdr->num_ref_idx_l0_active = 1;
    hdr->NumPocTotalCurr = 1;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }
  EXPECT_EQ(state_of_poc(0), UsedForLongTermReference);

  // Decode POC 14 and POC 15 (POC 16 will have LSB 0 again). The long-term
  // entry keeps identifying POC 0 by its LSB. The short-term entries of these
  // intermediate pictures are expressed relative to their own POC so the
  // previously decoded picture stays in the DPB instead of being evicted.
  //
  // POC 14 has no short-term predecessor in this sequence, so it carries only
  // the long-term entry. POC 15 references POC 14 as a short-term picture.
  {
    de265_image* p = begin_picture(14);
    ASSERT_NE(p, nullptr);
    auto hdr = make_header_with_st_rps({});
    add_long_term_entry(hdr.get(), 0, true, false);
    hdr->num_ref_idx_l0_active = 1;
    hdr->NumPocTotalCurr = 1;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
    EXPECT_EQ(state_of_poc(0), UsedForLongTermReference);
  }
  {
    de265_image* p = begin_picture(15);
    ASSERT_NE(p, nullptr);
    auto hdr = make_header_with_st_rps({{-1, true}});
    add_long_term_entry(hdr.get(), 0, true, false);
    hdr->num_ref_idx_l0_active = 2;
    hdr->NumPocTotalCurr = 2;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
    EXPECT_EQ(state_of_poc(0), UsedForLongTermReference);
    EXPECT_EQ(state_of_poc(14), UsedForShortTermReference);
  }

  de265_image* pic16 = begin_picture(16);
  ASSERT_NE(pic16, nullptr);
  tag_picture(pic16, 16);
  auto hdr = make_header_with_st_rps({{-1, true}});
  add_long_term_entry(hdr.get(), 0, true, false);
  hdr->slice_type = SLICE_TYPE_B;
  hdr->num_ref_idx_l0_active = 2;
  hdr->num_ref_idx_l1_active = 2;
  hdr->NumPocTotalCurr = 2;

  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());
  ASSERT_TRUE(run_reference_picture_lists(hdr.get()));

  // L0 order: short-term POC 15 first, long-term POC 0 second.
  EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{15, 0}));
  EXPECT_EQ(ref_pic_list_long_term_flags(hdr.get(), 0),
            (std::vector<bool>{false, true}));
  // L1 order: the only short-term current reference first, then the long-term
  // entry (clause 8.5.3.2.4).
  EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 1), (std::vector<int>{15, 0}));

  // The long-term list entry is the tagged POC-0 content, not POC 16.
  const de265_image* lt_target =
      ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[0][1]));
  ASSERT_NE(lt_target, nullptr);
  EXPECT_EQ(lt_target->PicOrderCntVal, 0);
  EXPECT_EQ(lt_target->PicState, UsedForLongTermReference);
  EXPECT_EQ(luma_value_at(lt_target, 0, 0), 100);
  // POC 16 is the current short-term picture and is untouched by LT marking.
  EXPECT_EQ(pic16->PicState, UsedForShortTermReference);
}

// A long-term reference carried with delta_poc_msb_present_flag == 1 is
// resolved through the full absolute-POC formula of clause 8.3.2 (step 1),
// which must still target the wrapped picture rather than the current one when
// both share the same poc_lsb_lt value.
TEST_F(RefPicManagementWrapTest, LongTermReferenceWithMsbCycleAcrossWrap) {
  // POC 0 is promoted to long term.
  de265_image* pic0 = begin_picture(0);
  ASSERT_NE(pic0, nullptr);
  tag_picture(pic0, 77);
  {
    auto hdr = make_header_with_st_rps({});
    add_long_term_entry(hdr.get(), /*poc_lsb_lt=*/0, /*used=*/true,
                        /*msb_present=*/false);
    hdr->num_ref_idx_l0_active = 1;
    hdr->NumPocTotalCurr = 1;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }

  // At POC 16 (LSB 0, MSB 16) the LT picture POC 0 is addressed with
  // poc_lsb_lt = 0 and delta_poc_msb_cycle_lt = 1:
  //   PocLt = lsb + (16 - 0) - 1 * 16 = 0.
  de265_image* pic16 = begin_picture(16);
  ASSERT_NE(pic16, nullptr);
  tag_picture(pic16, 16);
  auto hdr = make_header_with_st_rps({});
  add_long_term_entry(hdr.get(), /*poc_lsb_lt=*/0, /*used=*/true,
                      /*msb_present=*/true, /*delta_poc_msb_cycle=*/1);
  hdr->num_ref_idx_l0_active = 1;
  hdr->NumPocTotalCurr = 1;

  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());
  ASSERT_TRUE(run_reference_picture_lists(hdr.get()));

  ASSERT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{0}));
  const de265_image* target =
      ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[0][0]));
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->PicOrderCntVal, 0);
  EXPECT_EQ(target->PicState, UsedForLongTermReference);
  EXPECT_EQ(luma_value_at(target, 0, 0), 77);
  EXPECT_TRUE(hdr->LongTermRefPic[0][0]);
}

// ---------------------------------------------------------------------------
// 3. Short-term and long-term references coexisting
// ---------------------------------------------------------------------------

// One RPS that simultaneously keeps a short-term current reference, a following
// short-term picture, a used long-term reference and a following long-term
// picture. Each picture must end up with the marking state its RPS role
// dictates (clause 8.3.2).
TEST_F(RefPicManagementTest, MixedShortTermAndLongTermMarking) {
  // Seed the DPB with POC 0..3, all kept alive as short-term references.
  seed_retained_short_term_pictures(/*first_poc=*/0, /*count=*/4);
  for (int poc = 0; poc <= 3; ++poc) {
    de265_image* p = const_cast<de265_image*>(find_picture_by_poc(poc));
    ASSERT_NE(p, nullptr);
    tag_picture(p, static_cast<uint8_t>(20 + poc));
  }
  ASSERT_EQ(dpb_size(), 4u);

  // POC 4:
  //   POC 3 -> short-term, used by current
  //   POC 2 -> short-term, following only
  //   POC 1 -> long-term, used by current
  //   POC 0 -> long-term, following only
  de265_image* pic4 = begin_picture(4);
  ASSERT_NE(pic4, nullptr);
  auto hdr = make_header_with_st_rps({{-2, false}, {-1, true}});
  add_long_term_entry(hdr.get(), /*poc_lsb_lt=*/1, /*used=*/true,
                      /*msb_present=*/false);
  add_long_term_entry(hdr.get(), /*poc_lsb_lt=*/0, /*used=*/false,
                      /*msb_present=*/false);
  hdr->num_ref_idx_l0_active = 2;
  hdr->NumPocTotalCurr = 2;

  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());

  // Clause-8.3.2 marking expectations.
  EXPECT_EQ(state_of_poc(3), UsedForShortTermReference);  // StCurrBefore
  EXPECT_EQ(state_of_poc(2), UsedForShortTermReference);  // StFoll, still kept
  EXPECT_EQ(state_of_poc(1), UsedForLongTermReference);   // LtCurr
  EXPECT_EQ(state_of_poc(0), UsedForLongTermReference);   // LtFoll, still kept
  EXPECT_EQ(pic4->PicState, UsedForShortTermReference);   // current picture

  ASSERT_TRUE(run_reference_picture_lists(hdr.get()));
  // L0: short-term entries first, long-term entries last (clause 8.5.3.2.3).
  EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{3, 1}));
  EXPECT_EQ(ref_pic_list_long_term_flags(hdr.get(), 0),
            (std::vector<bool>{false, true}));

  // The LT entry points at POC 1's actual content.
  const de265_image* lt =
      ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[0][1]));
  ASSERT_NE(lt, nullptr);
  EXPECT_EQ(lt->PicOrderCntVal, 1);
  EXPECT_EQ(luma_value_at(lt, 0, 0), 21);
}

// A picture promoted to long-term stays long-term across subsequent pictures
// that no longer list it as short term: the marking transition must not be
// reverted by a later RPS.
TEST_F(RefPicManagementTest, LongTermMarkingPersistsUntilExplicitlyDropped) {
  de265_image* pic0 = begin_picture(0);
  ASSERT_NE(pic0, nullptr);
  {
    auto hdr = make_header_with_st_rps({});
    add_long_term_entry(hdr.get(), 0, true, false);
    hdr->num_ref_idx_l0_active = 1;
    hdr->NumPocTotalCurr = 1;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }
  EXPECT_EQ(state_of_poc(0), UsedForLongTermReference);

  // POC 1 references POC 0 purely as long term again.
  de265_image* pic1 = begin_picture(1);
  ASSERT_NE(pic1, nullptr);
  {
    auto hdr = make_header_with_st_rps({});
    add_long_term_entry(hdr.get(), 0, true, false);
    hdr->num_ref_idx_l0_active = 1;
    hdr->NumPocTotalCurr = 1;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }
  EXPECT_EQ(state_of_poc(0), UsedForLongTermReference);

  // POC 2 drops the long-term picture from every list: it becomes unused and
  // must be evicted, while POC 1 remains referenceable.
  de265_image* pic2 = begin_picture(2);
  ASSERT_NE(pic2, nullptr);
  {
    auto hdr = make_header_with_st_rps({{-1, true}});
    hdr->num_ref_idx_l0_active = 1;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }
  EXPECT_EQ(state_of_poc(0), UnusedForReference);
  EXPECT_EQ(state_of_poc(1), UsedForShortTermReference);
}

// ---------------------------------------------------------------------------
// 4. Eviction and reference list modification
// ---------------------------------------------------------------------------

// A referenced picture missing from the DPB (e.g. after packet loss) must be
// replaced by an unavailable-reference picture, and decoding of later pictures
// must keep working: the concealed placeholder is marked like the reference
// it stands in for and does not poison the reference lists of subsequent
// pictures that no longer need it.
TEST_F(RefPicManagementTest, MissingReferenceIsConcealedAndLaterEvicted) {
  // Only POC 0 is present; POC 1 was lost.
  de265_image* pic0 = begin_picture(0);
  ASSERT_NE(pic0, nullptr);
  tag_picture(pic0, 90);
  {
    auto hdr = make_header_with_st_rps({});
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());
  }

  const size_t dpb_before = dpb_size();

  // POC 2 references POC 1 (missing) and POC 0 (present).
  de265_image* pic2 = begin_picture(2);
  ASSERT_NE(pic2, nullptr);
  auto hdr = make_header_with_st_rps({{-2, true}, {-1, true}});
  hdr->num_ref_idx_l0_active = 2;
  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());

  // A placeholder for the missing POC 1 was generated inside the DPB.
  EXPECT_GT(dpb_size(), dpb_before);
  const de265_image* concealed = find_picture_by_poc(1);
  ASSERT_NE(concealed, nullptr);
  EXPECT_EQ(concealed->integrity, INTEGRITY_UNAVAILABLE_REFERENCE);
  EXPECT_EQ(concealed->PicState, UsedForShortTermReference);
  EXPECT_EQ(pic2->integrity, INTEGRITY_DERIVED_FROM_FAULTY_REFERENCE);

  ASSERT_TRUE(run_reference_picture_lists(hdr.get()));
  EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{0, 1}));

  // A later picture that references only POC 2 drops the placeholder from
  // every list, so it is evicted and cannot affect subsequent decoding.
  de265_image* pic3 = begin_picture(3);
  ASSERT_NE(pic3, nullptr);
  auto hdr3 = make_header_with_st_rps({{-1, true}});
  hdr3->num_ref_idx_l0_active = 1;
  ASSERT_EQ(run_reference_picture_set(hdr3.get()), DE265_OK);
  apply_evictions(hdr3.get());
  ASSERT_TRUE(run_reference_picture_lists(hdr3.get()));
  EXPECT_EQ(ref_pic_list_pocs(hdr3.get(), 0), (std::vector<int>{2}));

  const de265_image* stale = find_picture_by_poc(1);
  if (stale != nullptr) {
    EXPECT_EQ(stale->PicState, UnusedForReference);
  }
}

// Pictures absent from every RPS list of the current picture must leave the
// referenceable cache promptly, and the freed slot must be reusable without
// disturbing the surviving references.
TEST_F(RefPicManagementTest, UnreferencedPicturesAreEvictedAndSlotsReused) {
  // Fill with POC 0..3, keeping every earlier picture referenceable so all
  // four slots coexist in the DPB.
  seed_retained_short_term_pictures(0, 4);
  EXPECT_EQ(dpb_size(), 4u);

  // POC 4 references only POC 3. POC 0,1,2 are no longer in any list and must
  // be marked unused (then removed before the next slice decodes).
  de265_image* pic4 = begin_picture(4);
  ASSERT_NE(pic4, nullptr);
  auto hdr = make_header_with_st_rps({{-1, true}});
  hdr->num_ref_idx_l0_active = 1;
  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);

  // Before applying the eviction list they are scheduled for removal ...
  EXPECT_EQ(hdr->RemoveReferencesList.size(), 3u);
  apply_evictions(hdr.get());

  // ... afterwards they are unused and therefore releasable.
  for (int poc = 0; poc <= 2; ++poc) {
    EXPECT_EQ(state_of_poc(poc), UnusedForReference);
  }
  EXPECT_EQ(state_of_poc(3), UsedForShortTermReference);

  // Allocating another picture reuses a freed slot; POC 3 stays intact and is
  // still found by absolute-POC lookup.
  de265_image* pic5 = begin_picture(5);
  ASSERT_NE(pic5, nullptr);
  auto hdr5 = make_header_with_st_rps({{-2, true}});  // POC 5 -> POC 3
  hdr5->num_ref_idx_l0_active = 1;
  ASSERT_EQ(run_reference_picture_set(hdr5.get()), DE265_OK);
  apply_evictions(hdr5.get());
  ASSERT_TRUE(run_reference_picture_lists(hdr5.get()));

  EXPECT_EQ(ref_pic_list_pocs(hdr5.get(), 0), (std::vector<int>{3}));
  const de265_image* survivor =
      ctx()->get_image(static_cast<uint16_t>(hdr5->RefPicList[0][0]));
  EXPECT_EQ(survivor->PicOrderCntVal, 3);
  EXPECT_EQ(survivor->PicState, UsedForShortTermReference);

  // The evicted pictures are releasable (clause 8.3.2 end state): the DPB
  // reports free slots for them even though their storage is reclaimed lazily
  // on the next allocation.
  EXPECT_TRUE(ctx()->dpb.has_free_dpb_picture(/*high_priority=*/false));

  // The evicted POCs no longer resolve to any referenceable picture; any slot
  // that still holds their old data is in the releasable state rather than
  // being reachable through the reference machinery.
  for (int poc = 0; poc <= 2; ++poc) {
    const de265_image* stale = find_picture_by_poc(poc);
    if (stale != nullptr) {
      EXPECT_TRUE(stale->can_be_released());
      EXPECT_EQ(stale->PicState, UnusedForReference);
    }
  }

  // And the surviving reference is genuinely independent of the reused slots:
  // decoding another picture that only needs POC 3 must not resurrect any of
  // the evicted POCs into a reference list.
  de265_image* pic6 = begin_picture(6);
  ASSERT_NE(pic6, nullptr);
  auto hdr6 = make_header_with_st_rps({{-3, true}});  // POC 6 -> POC 3
  hdr6->num_ref_idx_l0_active = 1;
  ASSERT_EQ(run_reference_picture_set(hdr6.get()), DE265_OK);
  apply_evictions(hdr6.get());
  ASSERT_TRUE(run_reference_picture_lists(hdr6.get()));
  EXPECT_EQ(ref_pic_list_pocs(hdr6.get(), 0), (std::vector<int>{3}));
}

// RefPicListModification reorders the temp list: the modified entries must
// still reference the same underlying pictures with the correct LT flags.
TEST_F(RefPicManagementTest, ReferenceListModificationPreservesPictureIdentity) {
  seed_retained_short_term_pictures(0, 3);
  for (int poc = 0; poc <= 2; ++poc) {
    de265_image* p = const_cast<de265_image*>(find_picture_by_poc(poc));
    ASSERT_NE(p, nullptr);
    tag_picture(p, static_cast<uint8_t>(40 + poc));
  }

  // POC 1 promoted to long term.
  de265_image* pic3 = begin_picture(3);
  ASSERT_NE(pic3, nullptr);
  {
    auto hdr = make_header_with_st_rps({{-1, true}, {-3, true}});  // POC 2, POC 0
    add_long_term_entry(hdr.get(), /*poc_lsb_lt=*/1, /*used=*/true,
                        /*msb_present=*/false);                      // POC 1 LT
    hdr->num_ref_idx_l0_active = 3;
    hdr->NumPocTotalCurr = 3;
    ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
    apply_evictions(hdr.get());

    // Temp L0 order before modification: ST-before {2,0}, then LT {1}.
    // Request explicit order [LT(1), ST(2), ST(0)] via list_entry_l0.
    hdr->ref_pic_list_modification_flag_l0 = true;
    hdr->list_entry_l0[0] = 2;  // temp index 2 -> long-term POC 1
    hdr->list_entry_l0[1] = 0;  // temp index 0 -> POC 2
    hdr->list_entry_l0[2] = 1;  // temp index 1 -> POC 0

    ASSERT_TRUE(run_reference_picture_lists(hdr.get()));
    EXPECT_EQ(ref_pic_list_pocs(hdr.get(), 0), (std::vector<int>{1, 2, 0}));
    EXPECT_EQ(ref_pic_list_long_term_flags(hdr.get(), 0),
              (std::vector<bool>{true, false, false}));

    const std::vector<uint8_t> expected_tags = {41, 42, 40};
    for (int i = 0; i < 3; ++i) {
      const de265_image* target =
          ctx()->get_image(static_cast<uint16_t>(hdr->RefPicList[0][i]));
      ASSERT_NE(target, nullptr);
      EXPECT_EQ(luma_value_at(target, 0, 0), expected_tags[i]);
    }
  }
}

// An IDR picture empties all current/following RPS state: after it is
// processed, pre-IDR pictures must not be reachable as references any more.
TEST_F(RefPicManagementTest, IdrPictureClearsReferenceSets) {
  // Three retained short-term pictures that the IDR must invalidate.
  seed_retained_short_term_pictures(0, 3);

  // Simulate the IDR branch taken in process_reference_picture_set().
  ctx()->nal_unit_type = NAL_UNIT_IDR_W_RADL;
  ctx()->IdrPicFlag = true;
  ctx()->RapPicFlag = true;
  ctx()->NoRaslOutputFlag = true;

  de265_image* idr = begin_picture(16);
  ASSERT_NE(idr, nullptr);
  auto hdr = make_header_with_st_rps({});
  ASSERT_EQ(run_reference_picture_set(hdr.get()), DE265_OK);
  apply_evictions(hdr.get());

  // The internal RPS bookkeeping is reset ...
  EXPECT_EQ(ctx()->NumPocStCurrBefore, 0);
  EXPECT_EQ(ctx()->NumPocStCurrAfter, 0);
  EXPECT_EQ(ctx()->NumPocStFoll, 0);
  EXPECT_EQ(ctx()->NumPocLtCurr, 0);
  EXPECT_EQ(ctx()->NumPocLtFoll, 0);

  // ... and every older picture was scheduled for eviction.
  for (int poc = 0; poc <= 2; ++poc) {
    EXPECT_EQ(state_of_poc(poc), UnusedForReference);
  }
  // The IDR picture itself stays referenceable.
  EXPECT_EQ(idr->PicState, UsedForShortTermReference);
}
