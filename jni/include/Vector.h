#pragma once

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;
};

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    float m[16];
};

struct Matrix34 {
    Vec4 translation, rotation, scale;
};
