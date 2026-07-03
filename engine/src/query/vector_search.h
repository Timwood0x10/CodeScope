#ifndef VECTOR_SEARCH_H
#define VECTOR_SEARCH_H

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>

/**
 * Minimal character n-gram vector search.
 *
 * Converts strings to fixed-size hash vectors using character
 * n-grams (2-grams and 3-grams). Search finds the most similar
 * vectors by cosine similarity — no external ML dependencies.
 *
 * This gives "semantic-ish" search: "add" matches "adder",
 * "addition", "summation" better than FTS5 prefix alone.
 */

namespace vector_search {

// ─── Vector type ──────────────────────────────────────────────

using Vector = std::vector<float>;

// ─── Configuration ────────────────────────────────────────────

constexpr int VECTOR_DIM = 64;     // Fixed vector dimension

// ─── Functions ────────────────────────────────────────────────

/**
 * Convert a string to a fixed-size n-gram hash vector.
 *
 * Algorithm:
 *   1. Extract all 2-grams and 3-grams from the string
 *   2. Hash each n-gram to an index (0..VECTOR_DIM-1)
 *   3. Build a frequency vector of n-gram occurrences
 *   4. L2-normalize the vector
 *
 * @param text  Input string (symbol name, identifier, etc.)
 * @return Fixed-size float vector
 */
Vector stringToVector(const std::string& text);

/**
 * Compute cosine similarity between two vectors.
 *
 * @return Similarity in [0, 1], 1 = identical
 */
float cosineSimilarity(const Vector& a, const Vector& b);

/**
 * Serialize a vector to a byte string for SQLite BLOB storage.
 */
std::string serializeVector(const Vector& v);

/**
 * Deserialize a BLOB back to a vector.
 */
Vector deserializeVector(const std::string& blob);

} // namespace vector_search

#endif // VECTOR_SEARCH_H
