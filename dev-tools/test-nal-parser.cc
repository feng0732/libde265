/*
 * H.265 video codec.
 * Copyright (c) 2026 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tests.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "libde265/nal-parser.h"

#include <stdio.h>
#include <stdint.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Tests for the NAL-unit boundary handling of NAL_Parser.
//
// The decoder accepts the same NAL units either delimited by start codes
// (de265_push_data, Annex-B byte stream) or length-prefixed (de265_push_NAL), and
// input arrives in arbitrarily sized fragments. The parser must produce the same
// units, in the same order, regardless of fragmentation or of the encapsulation
// used, and must never merge two units into one. An incomplete trailing unit is
// dropped on flush_data() and must not swallow the unit that follows it.

namespace {

typedef std::vector<std::string> NalList;

NalList drain(NAL_Parser& parser)
{
  NalList out;
  while (std::unique_ptr<NAL_unit> nal = parser.pop_from_NAL_queue()) {
    out.emplace_back(reinterpret_cast<const char*>(nal->data()), nal->size());
  }
  return out;
}

std::string to_hex(const std::string& s)
{
  std::string r;
  char b[4];
  for (unsigned char c : s) { snprintf(b, sizeof b, "%02x", c); r += b; }
  return r;
}

void print_nals(const char* tag, const NalList& nals)
{
  printf("    %s: %zu NAL unit(s)\n", tag, nals.size());
  for (const std::string& s : nals) printf("      [%s]\n", to_hex(s).c_str());
}

bool lists_equal(const NalList& a, const NalList& b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i] != b[i]) return false;
  return true;
}

// A small start-code stream with three units; the second one contains an
// emulation-prevention byte (00 00 03) that must survive byte-stream parsing.
std::string make_stream()
{
  const unsigned char n1[] = { 0x40, 0x01, 0xaa, 0xbb };
  const unsigned char n2[] = { 0x42, 0x01, 0x00, 0x00, 0x03, 0x05 };
  const unsigned char n3[] = { 0x44, 0x01, 0x10, 0x11, 0x12 };

  std::string s;
  const unsigned char* units[] = { n1, n2, n3 };
  const int sizes[] = { (int)sizeof(n1), (int)sizeof(n2), (int)sizeof(n3) };
  for (int i = 0; i < 3; i++) {
    s += std::string("\x00\x00\x01", 3);
    s += std::string(reinterpret_cast<const char*>(units[i]), sizes[i]);
  }
  return s;
}

NalList parse_bytestream(const std::string& stream, int chunk)
{
  NAL_Parser parser;
  for (size_t i = 0; i < stream.size(); i += chunk) {
    int n = (int)std::min((size_t)chunk, stream.size() - i);
    parser.push_data(reinterpret_cast<const unsigned char*>(stream.data()) + i, n, 0);
  }
  parser.flush_data();
  return drain(parser);
}

} // namespace


class NalParserTest : public Test
{
 public:
  const char* getName() const override { return "nal-parser"; }
  const char* getDescription() const override { return "NAL unit boundary handling (fragments, encapsulation switch)"; }

  bool work(bool quiet) override
  {
    bool ok = true;

    auto expect = [&](bool cond, const char* what) {
      if (!cond) { ok = false; printf("  FAIL: %s\n", what); }
      else if (!quiet) printf("  ok:   %s\n", what);
    };

    const std::string stream = make_stream();

    const NalList reference = parse_bytestream(stream, (int)stream.size());
    expect(reference.size() == 3, "unfragmented stream yields three NAL units");
    if (reference.size() != 3) print_nals("got", reference);

    // --- fragmentation split at every possible chunk size ---
    for (int chunk : { 1, 2, 3, 4, 5, 7, 11 }) {
      NalList got = parse_bytestream(stream, chunk);
      char what[96];
      snprintf(what, sizeof what, "fragmented byte stream (chunk=%d) matches unfragmented", chunk);
      expect(lists_equal(got, reference), what);
      if (!lists_equal(got, reference)) print_nals("got", got);
    }

    // --- an incomplete unit is dropped and must not merge with the next one ---
    {
      NAL_Parser parser;
      const unsigned char fragment[] = { 0x00, 0x00, 0x01, 0x40 };  // start code + 1 header byte
      parser.push_data(fragment, sizeof fragment, 0);
      parser.flush_data();

      expect(parser.number_of_NAL_units_pending() == 0,
             "incomplete NAL unit is not pending after flush");
      expect(parser.bytes_in_input_queue() == 0,
             "no input bytes reported after flush of an incomplete NAL unit");

      const unsigned char next[] = { 0x00, 0x00, 0x01, 0x42, 0x01, 0x99 };
      parser.push_data(next, sizeof next, 0);
      parser.flush_data();

      NalList got = drain(parser);
      expect(got.size() == 1, "input after an incomplete unit yields one NAL unit");
      if (got.size() == 1) {
        expect(to_hex(got[0]) == "420199", "resumed NAL unit is not merged with the dropped fragment");
      } else print_nals("got", got);
    }

    // --- switch from byte stream to length-prefixed units ---
    {
      NAL_Parser parser;
      const unsigned char fragment[] = { 0x00, 0x00, 0x01, 0x40, 0x01, 0xaa }; // no following start code
      parser.push_data(fragment, sizeof fragment, 0);

      const unsigned char nal2[] = { 0x42, 0x01, 0xbb };
      expect(parser.push_NAL(nal2, sizeof nal2, 0) == DE265_OK,
             "push_NAL after pending byte-stream data succeeds");
      const unsigned char nal3[] = { 0x44, 0x01, 0xcc };
      expect(parser.push_NAL(nal3, sizeof nal3, 0) == DE265_OK,
             "consecutive push_NAL succeeds");

      NalList got = drain(parser);
      expect(got.size() == 3, "encapsulation switch keeps all three NAL units");
      if (got.size() == 3) {
        expect(to_hex(got[0]) == "4001aa", "pending byte-stream unit comes first");
        expect(to_hex(got[1]) == "4201bb", "first length-prefixed unit comes second");
        expect(to_hex(got[2]) == "4401cc", "second length-prefixed unit comes third");
      } else print_nals("got", got);
    }

    // --- switch from length-prefixed units back to the byte stream ---
    {
      NAL_Parser parser;
      const unsigned char nal1[] = { 0x40, 0x01, 0xaa };
      parser.push_NAL(nal1, sizeof nal1, 0);

      const unsigned char fragment[] = { 0x00, 0x00, 0x01, 0x42, 0x01, 0xbb };
      parser.push_data(fragment, sizeof fragment, 0);
      parser.flush_data();

      NalList got = drain(parser);
      expect(got.size() == 2, "switching back to the byte stream keeps both NAL units");
      if (got.size() == 2) {
        expect(to_hex(got[0]) == "4001aa", "length-prefixed unit comes first");
        expect(to_hex(got[1]) == "4201bb", "byte-stream unit comes second");
      } else print_nals("got", got);
    }

    // --- a rejected push_NAL must not disturb pending byte-stream data ---
    {
      NAL_Parser parser;
      const unsigned char fragment[] = { 0x00, 0x00, 0x01, 0x40, 0x01, 0xaa };
      parser.push_data(fragment, sizeof fragment, 0);

      const unsigned char too_short[] = { 0x40 };
      expect(parser.push_NAL(too_short, sizeof too_short, 0) == DE265_ERROR_INVALID_ARGUMENT,
             "one-byte push_NAL is rejected with INVALID_ARGUMENT");

      const unsigned char nal2[] = { 0x42, 0x01, 0xbb };
      parser.push_NAL(nal2, sizeof nal2, 0);
      parser.flush_data();

      NalList got = drain(parser);
      expect(got.size() == 2, "rejected push_NAL leaves the following units unmerged");
      if (got.size() == 2) {
        expect(to_hex(got[0]) == "4001aa", "pending unit intact after rejection");
        expect(to_hex(got[1]) == "4201bb", "following unit intact after rejection");
      } else print_nals("got", got);
    }

    if (!quiet) printf("NAL parser boundary checks: %s\n", ok ? "all passed" : "FAILURES");
    return ok;
  }
} nalParserTest;
