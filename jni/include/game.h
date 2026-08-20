#pragma once
#include <sys/types.h>
#include <vector>

// Max bones collected per player for the skeleton ESP.
// The dump ragdoll (Ragdoll.cGP / cGw) exposes the full body
// bone set (hips, spine, chest, head, arms, legs, hands, feet...);
// anything above this cap is truncated.
inline constexpr int ESP_MAX_BONES = 48;

struct EspBox {
    float x1, y1, x2, y2;
    float distance;
    float corners[8][2];
    bool  corner_visible[8];

    // --- Skeleton ESP (bones resolved from the dump ragdoll chain) ---
    int   bone_count = 0;                 // valid entries in bones[]
    float bones[ESP_MAX_BONES][2];        // screen-space bone positions
    bool  bone_valid[ESP_MAX_BONES]{};    // projection succeeded + on screen
    int   bone_edge_count = 0;            // valid entries in bone_edges[]
    int   bone_edges[ESP_MAX_BONES][2];   // pairs of bone indexes (child -> parent bone)
};

bool        esp_init(pid_t pid);
void        esp_reset();
std::vector<EspBox> esp_get_boxes(int screen_width, int screen_height, bool collect_skeleton = false);
