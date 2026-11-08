#ifndef CODESCOPE_STORE_IDENTITY_H
#define CODESCOPE_STORE_IDENTITY_H

#include <cstdint>
#include <string>
#include <string_view>

namespace store
{

/// SHA-256 of `data` as 64 lowercase hex characters.
///
/// Identity keys must be content-derived and stable across processes, so a
/// 32-bit hash (the migration checksums use FNV-1a) is not enough here: a
/// collision would merge two different findings.
std::string Sha256Hex(std::string_view data);

/// Stable identity of a finding: sha256(rule_id + "\n" + anchor).
///
/// The rule VERSION is deliberately excluded: bumping a rule would otherwise
/// turn every existing finding into a new one. The newline separator keeps
/// ("ab", "c") from colliding with ("a", "bc").
///
/// `anchor` MUST NOT contain a newline: the newline is the field separator,
/// so an anchor carrying one would let ("r", "x\ny") collide with a different
/// (rule, anchor) pair. Anchors are built from symbol / edge / module / file
/// descriptors (development_plan §17.6), none of which may embed a newline.
/// See development_plan §17.6.
std::string FindingKey(const std::string &rule_id, const std::string &anchor);

} // namespace store

#endif // CODESCOPE_STORE_IDENTITY_H
