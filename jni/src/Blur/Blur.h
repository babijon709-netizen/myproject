#pragma once
#include <GLES3/gl3.h>
#include "ImGui/imgui.h"

namespace Blur {

extern GLuint s_fbo[3];
extern GLuint s_tex[3];
extern GLuint s_progH;
extern GLuint s_progV;
extern GLuint s_vao;
extern GLuint s_vbo;
extern int    s_mw;
extern int    s_mh;
extern bool   s_ready;
extern bool   s_frozen;

void Init();
void Freeze(int screenW, int screenH, int menuX, int menuY, int menuW, int menuH);
void Unfreeze();
void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float alpha, float rounding);
void Free();

}