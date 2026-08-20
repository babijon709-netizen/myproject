#pragma once
#include <cstdint>

// ============================================================
// Offsets taken from the il2cpp dump (dump.cs / dump 1.7z)
// Game: HyperHug Oxide (Oxide namespace, Assembly-CSharp.dll)
// ============================================================
namespace game_offsets {

inline constexpr std::uint64_t CAMERA_PROJECTION_MATRIX = 0x140;
inline constexpr std::uint64_t CAMERA_VIEW_MATRIX       = 0x2F8;

inline constexpr std::uint64_t MANAGED_CACHED_PTR = 0x10;

inline constexpr float PLAYER_HEIGHT          = 1.8F;
inline constexpr float PLAYER_BOX_WIDTH_RATIO = 0.40F;
inline constexpr float MIN_PLAYER_DISTANCE    = 0.0F;
inline constexpr float MAX_PLAYER_DISTANCE    = 300.0F;

inline constexpr std::uint64_t PLAYER_MANAGER_TYPEINFO_RVA       = 0xD126870;
inline constexpr std::uint64_t PLAYER_MANAGER_STATIC_FIELDS_LIST = 0x10;

inline constexpr std::uint64_t GAME_CONTROLLER_TYPEINFO_RVA        = 0xD121CF8;
inline constexpr std::uint64_t GAME_CONTROLLER_LOCAL_PLAYER_FIELD  = 0x10;
inline constexpr std::uint64_t GAME_CONTROLLER_CAMERA_MANAGER_FIELD = 0x38;
inline constexpr std::uint64_t CAMERA_MANAGER_CAMERA_FIELD          = 0x20;

inline constexpr std::uint64_t PLAYER_TRANSFORM = 0x68;
inline constexpr std::uint64_t PLAYER_POSITION  = 0x1D0;

inline constexpr std::uint64_t IL2CPP_LIST_ITEMS          = 0x10;
inline constexpr std::uint64_t IL2CPP_LIST_SIZE           = 0x18;
inline constexpr std::uint64_t IL2CPP_ARRAY_FIRST_ELEMENT = 0x20;
inline constexpr std::uint64_t IL2CPP_ARRAY_LENGTH        = 0x18;

// ------------------------------------------------------------
// Skeleton bones (all offsets verified against dump.cs)
//
// Managed chain (PlayerManager -> bone Transforms):
//   PlayerManager.Txa                       : qL               @ 0x248
//   qL.<HYD>k__BackingField                 : PlayerModelInfo  @ 0x10
//   PlayerModelInfo.characterAnimation      : CharacterAnimation @ 0x60
//   CharacterAnimation.ragdoll              : Ragdoll          @ 0x38
//   Ragdoll.cGP                             : List<BodyPart>   @ 0xA0
//   Ragdoll.BodyPart.transform              : Transform        @ 0x10
//
// Fallback bone source (same Ragdoll):
//   Ragdoll.cGw : Dictionary<Transform, LocalTRS>            @ 0xA8
//     -> keys are the same bone Transforms
// ------------------------------------------------------------
inline constexpr std::uint64_t PLAYER_MODEL_BAG                 = 0x248;
inline constexpr std::uint64_t MODEL_BAG_MODEL_INFO             = 0x10;
inline constexpr std::uint64_t MODEL_INFO_CHARACTER_ANIMATION   = 0x60;
inline constexpr std::uint64_t CHARACTER_ANIMATION_RAGDOLL      = 0x38;
inline constexpr std::uint64_t RAGDOLL_BODY_PARTS_LIST          = 0xA0;
inline constexpr std::uint64_t RAGDOLL_BONE_LOCALS_DICTIONARY   = 0xA8;
inline constexpr std::uint64_t BODY_PART_TRANSFORM              = 0x10;

// System.Collections.Generic.Dictionary<TKey, TValue> managed layout:
//   header 0x10 | _buckets 0x10 | _entries 0x18 | _count 0x20 | ...
// Entry<Transform, LocalTRS> (struct, inside array at 0x20):
//   hashCode 0x0 (int) | next 0x4 (int) | key 0x8 (Transform*) | value 0x10 (LocalTRS = 40 bytes)
inline constexpr std::uint64_t DICTIONARY_ENTRIES               = 0x18;
inline constexpr std::uint64_t DICTIONARY_COUNT                 = 0x20;
inline constexpr std::uint64_t DICTIONARY_ENTRY_SIZE            = 0x38;
inline constexpr std::uint64_t DICTIONARY_ENTRY_HASHCODE        = 0x0;
inline constexpr std::uint64_t DICTIONARY_ENTRY_KEY             = 0x8;

inline constexpr int MAX_DUMP_BONES = 64;

}
