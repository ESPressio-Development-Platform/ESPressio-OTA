#include <array>
#include <cstddef>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;
using CheckpointStore = ArtifactCheckpointStore<Capacity>;

constexpr bool CheckAuthoritativeCheckpointKeys() noexcept {
    std::array<ESPressio::Persistence::AtomicRecordKey,
               Capacity::MaximumArtifactCheckpoints> keys{};

    for (std::size_t slot = 0U; slot < keys.size(); ++slot) {
        if (!CheckpointStore::TryRecordKey(slot, keys[slot]) || !keys[slot]) return false;
    }

    for (std::size_t left = 0U; left < keys.size(); ++left) {
        for (std::size_t right = left + 1U; right < keys.size(); ++right) {
            if (keys[left] == keys[right]) return false;
        }
    }

    ESPressio::Persistence::AtomicRecordKey repeated{};
    if (!CheckpointStore::TryRecordKey(2U, repeated) || !(repeated == keys[2U])) return false;

    ESPressio::Persistence::AtomicRecordKey outOfRange{};
    if (CheckpointStore::TryRecordKey(keys.size(), outOfRange) || outOfRange) return false;

    return true;
}

static_assert(CheckAuthoritativeCheckpointKeys(),
              "Artifact checkpoint durable keys must be finite, stable and pairwise distinct");

} // namespace

int main() {
    return CheckAuthoritativeCheckpointKeys() ? 0 : 1;
}
