// Mission 10: unit test for the push-constant trace's formatting, sampling and correlation
// (pure logic, no GPU, no game launch).

#include "prx/libSceAgcDriver/Graphics/include/PushTrace.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace AgcDriver::Graphics::PushTrace;

    // The exact block: a known 20-byte pattern, its hexadecimal form, its words and its float
    // views. The float view is a bit_cast of the same bytes, never a reinterpretation of the
    // source type.
    std::vector<std::uint8_t> block(20);
    for (std::size_t index = 0; index < block.size(); ++index) {
        block[index] = static_cast<std::uint8_t>(index * 11 + 3);
    }
    const auto hex = Hex(block.data(), block.size());
    Require(hex.size() == 40, "the hexadecimal form is two characters per byte");
    std::string expectedHex;
    std::string expectedWords;
    char buffer[16];
    for (std::size_t index = 0; index < block.size(); ++index) {
        std::snprintf(buffer, sizeof(buffer), "%02x", block[index]);
        expectedHex += buffer;
        if (index % 4 == 0 && index != 0) expectedWords += ",";
        if (index % 4 == 0) {
            std::uint32_t word = 0;
            std::memcpy(&word, block.data() + index, sizeof(word));
            std::snprintf(buffer, sizeof(buffer), "0x%08x", word);
            expectedWords += buffer;
        }
    }
    Require(hex == expectedHex, "the hexadecimal form is the bytes in order");
    const auto words = Words(block.data(), block.size());
    Require(words == expectedWords, "the words are the bytes read as dwords in order");
    const auto floats = Floats(block.data(), block.size());
    Require(!floats.empty(), "the float view is produced");
    Require(floats.find("nan") == std::string::npos && floats.find("inf") == std::string::npos,
            "a non-finite bit pattern is printed as a number, not as a claim");

    // The fingerprint: stable for the same bytes, different when a byte changes.
    const auto fingerprint = Fingerprint(block.data(), block.size());
    Require(fingerprint == Fingerprint(block.data(), block.size()), "the fingerprint is stable");
    auto changed = block;
    changed[7] = static_cast<std::uint8_t>(changed[7] ^ 1u);
    Require(fingerprint != Fingerprint(changed.data(), changed.size()), "the fingerprint changes with a byte");

    // The identity filter: the two known programs by default, and the mismatch counted.
    Require(IsTraced("dbcdd752b39dc3421aa7a26d998f06f7"), "the first known program is traced");
    Require(IsTraced("9bc3e517b32e7bdd4dba94778e654067"), "the second known program is traced");
    Require(!IsTraced("00000000000000000000000000000000"), "an unknown program is not traced");
    // The identity is two 64-bit words; the draw-shader event prints the low word first, so
    // the strings of the two diagnostics match. The values are the ones Mission 9 measured.
    Require(IdentityText(0xdbcdd752b39dc342ull, 0x1aa7a26d998f06f7ull) ==
                "dbcdd752b39dc3421aa7a26d998f06f7",
            "the identity is the low word then the high word, as the draw-shader event prints it");
    Require(IdentityText(0xdbcdd752b39dc342ull, 0x1aa7a26d998f06f7ull) ==
                "dbcdd752b39dc3421aa7a26d998f06f7",
            "the traced identity is the string the draw-shader event printed");

    // The sampling: the first sample, then one per five-second bucket, then the changed
    // values, with the caps.
    Sampler sampler;
    Sampler::Reason reason = Sampler::Reason::None;
    Require(sampler.Admit("a", 1, 0, reason) && reason == Sampler::Reason::First, "the first sample is taken");
    Require(!sampler.Admit("a", 1, 100, reason) && reason == Sampler::Reason::None, "the same value in the same bucket is not sampled");
    Require(sampler.Admit("a", 1, 5000, reason) && reason == Sampler::Reason::Bucket, "the next bucket is sampled");
    Require(sampler.Admit("a", 2, 5001, reason) && reason == Sampler::Reason::Changed, "a changed value is sampled");
    Require(!sampler.Admit("a", 2, 5002, reason), "the same changed value is not sampled again");
    const auto counts = sampler.CountsOf("a");
    Require(counts.scheduled >= 2, "the scheduled samples are counted");
    Require(counts.changed == 1, "the changed samples are counted");
    Require(counts.suppressed >= 2, "the suppressed samples are counted");
    Require(sampler.CountsOf("missing").scheduled == 0, "an unknown identity has no counts");

    // The caps: the scheduled samples stop at the limit and the changed ones at theirs.
    Sampler capped;
    std::uint64_t taken = 0;
    for (std::uint64_t elapsed = 0; elapsed < Sampler::BucketMilliseconds * 200; elapsed += Sampler::BucketMilliseconds) {
        if (capped.Admit("b", 1, elapsed, reason)) ++taken;
    }
    Require(taken <= Sampler::ScheduledLimit, "the scheduled samples are capped");
    Sampler changedCap;
    std::uint64_t changes = 0;
    for (std::uint64_t index = 0; index < 100; ++index) {
        if (changedCap.Admit("c", index + 1, index * 10, reason)) ++changes;
    }
    Require(changes <= Sampler::ChangedLimit + 1, "the changed samples are capped");

    // The correlation: the ids are monotonic and the pending record holds the block.
    const auto first = Correlator::NextCorrelation();
    const auto second = Correlator::NextCorrelation();
    Require(second > first, "the correlation ids increase");
    auto& pending = Correlator::Current();
    pending.active = true;
    pending.correlation = first;
    pending.identity = "abc";
    pending.offset = 16;
    pending.bytes = block;
    pending.fingerprint = fingerprint;
    pending.uploaded = false;
    Require(pending.active && pending.correlation == first && pending.identity == "abc" && pending.offset == 16,
            "the pending record keeps the block's identity");
    Require(pending.bytes.size() == 20 && pending.bytes == block, "the pending record keeps the exact bytes");
    Require(pending.fingerprint == fingerprint, "the pending record keeps the fingerprint");
    pending.uploaded = true;
    Require(pending.uploaded, "the upload stage is recorded");
    Require(Correlator::Unmatched() == 0, "no correlation is unmatched yet");

    // The timestamps: the monotonic origin and the elapsed time do not go backwards.
    const auto mono = MonoNanoseconds();
    const auto elapsed = ElapsedMilliseconds();
    Require(mono <= MonoNanoseconds() + 100000000ull, "the monotonic clock does not go backwards");
    Require(elapsed <= 1000000ull, "the elapsed time is in milliseconds");
    Require(Origin() == Origin(), "the origin is a single clock");

    std::printf("push trace tests passed\n");
    return 0;
}
