#include "vector_search.h"

#include <cstring>
#include <unordered_map>
#include <algorithm>

namespace vector_search {

// ─── Jenkins one-at-a-time hash ───────────────────────────────

static uint32_t hash32(const char* data, size_t len) {
    uint32_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h += static_cast<unsigned char>(data[i]);
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

// ─── String to n-gram → vector ────────────────────────────────

Vector stringToVector(const std::string& text) {
    Vector vec(VECTOR_DIM, 0.0f);
    if (text.empty()) return vec;

    // Lowercase for case-insensitivity
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Extract 2-grams and 3-grams
    auto addNGram = [&vec](const std::string& gram) {
        if (gram.empty()) return;
        uint32_t idx = hash32(gram.data(), gram.size()) % VECTOR_DIM;
        vec[idx] += 1.0f;
    };

    if (lower.size() >= 2) {
        for (size_t i = 0; i + 1 < lower.size(); i++) {
            addNGram(lower.substr(i, 2));
        }
    }
    if (lower.size() >= 3) {
        for (size_t i = 0; i + 2 < lower.size(); i++) {
            addNGram(lower.substr(i, 3));
        }
    }

    // L2 normalize
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (float& v : vec) v /= norm;
    }

    return vec;
}

// ─── Cosine similarity ────────────────────────────────────────

float cosineSimilarity(const Vector& a, const Vector& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;

    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    na = std::sqrt(na);
    nb = std::sqrt(nb);
    if (na == 0.0f || nb == 0.0f) return 0.0f;
    return dot / (na * nb);
}

// ─── Serialization ────────────────────────────────────────────

std::string serializeVector(const Vector& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
}

Vector deserializeVector(const std::string& blob) {
    if (blob.size() != VECTOR_DIM * sizeof(float)) return Vector(VECTOR_DIM, 0.0f);
    Vector v(VECTOR_DIM);
    std::memcpy(v.data(), blob.data(), blob.size());
    return v;
}

} // namespace vector_search
