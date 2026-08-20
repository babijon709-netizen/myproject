#include "Blur/Blur.h"

namespace Blur {

GLuint s_fbo[3] = {};
GLuint s_tex[3] = {};
GLuint s_progH  = 0;
GLuint s_progV  = 0;
GLuint s_vao    = 0;
GLuint s_vbo    = 0;
int    s_mw     = 0;
int    s_mh     = 0;
bool   s_ready  = false;
bool   s_frozen = false;

static const char* kVS = R"(#version 300 es
layout(location=0) in vec2 p;
out vec2 uv;
void main(){ uv=p*.5+.5; gl_Position=vec4(p,0,1); }
)";

static const char* kFSH = R"(#version 300 es
precision highp float;
in vec2 uv; out vec4 c;
uniform sampler2D t;
uniform float d;
void main(){
    c  = texture(t,uv+vec2(-3.*d,0.))*0.0625;
    c += texture(t,uv+vec2(-2.*d,0.))*0.125;
    c += texture(t,uv+vec2(-1.*d,0.))*0.25;
    c += texture(t,uv              )*0.125;
    c += texture(t,uv+vec2( 1.*d,0.))*0.25;
    c += texture(t,uv+vec2( 2.*d,0.))*0.125;
    c += texture(t,uv+vec2( 3.*d,0.))*0.0625;
}
)";

static const char* kFSV = R"(#version 300 es
precision highp float;
in vec2 uv; out vec4 c;
uniform sampler2D t;
uniform float d;
void main(){
    c  = texture(t,uv+vec2(0.,-3.*d))*0.0625;
    c += texture(t,uv+vec2(0.,-2.*d))*0.125;
    c += texture(t,uv+vec2(0.,-1.*d))*0.25;
    c += texture(t,uv              )*0.125;
    c += texture(t,uv+vec2(0., 1.*d))*0.25;
    c += texture(t,uv+vec2(0., 2.*d))*0.125;
    c += texture(t,uv+vec2(0., 3.*d))*0.0625;
}
)";

static GLuint mkShader(GLenum t, const char* s){
    GLuint sh=glCreateShader(t);
    glShaderSource(sh,1,&s,nullptr);
    glCompileShader(sh);
    return sh;
}
static GLuint mkProg(const char* vs, const char* fs){
    GLuint v=mkShader(GL_VERTEX_SHADER,vs);
    GLuint f=mkShader(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram();
    glAttachShader(p,v); glAttachShader(p,f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}
static void mkBuf(int i, int w, int h){
    if(s_tex[i]){ glDeleteTextures(1,&s_tex[i]); s_tex[i]=0; }
    if(s_fbo[i]){ glDeleteFramebuffers(1,&s_fbo[i]); s_fbo[i]=0; }
    glGenTextures(1,&s_tex[i]);
    glBindTexture(GL_TEXTURE_2D,s_tex[i]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1,&s_fbo[i]);
    glBindFramebuffer(GL_FRAMEBUFFER,s_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s_tex[i],0);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
}
static void quad(){
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glBindVertexArray(0);
}

void Init(){
    s_frozen=false;
    s_mw=0; s_mh=0;
    s_progH=mkProg(kVS,kFSH);
    s_progV=mkProg(kVS,kFSV);
    float q[]={-1,-1,1,-1,-1,1,1,1};
    glGenVertexArrays(1,&s_vao);
    glGenBuffers(1,&s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER,s_vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(q),q,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
    glBindVertexArray(0);
    s_ready=true;
}

void Freeze(int screenW, int screenH, int menuX, int menuY, int menuW, int menuH){
    if(!s_ready||s_frozen) return;

    int mw = menuW;
    int mh = menuH;
    int hw = mw/2;
    int hh = mh/2;

    if(mw!=s_mw||mh!=s_mh){
        s_mw=mw; s_mh=mh;
        mkBuf(0,mw,mh);
        mkBuf(1,hw,hh);
        mkBuf(2,hw,hh);
    }

    GLint prevFBO=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prevFBO);
    GLint vp[4];     glGetIntegerv(GL_VIEWPORT,vp);
    GLboolean blend; glGetBooleanv(GL_BLEND,&blend);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);

    int srcX0 = menuX;
    int srcY0 = screenH - menuY - mh;
    int srcX1 = menuX + mw;
    int srcY1 = screenH - menuY;

    glBindFramebuffer(GL_READ_FRAMEBUFFER,prevFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s_fbo[0]);
    glBlitFramebuffer(srcX0,srcY1,srcX1,srcY0, 0,0,mw,mh, GL_COLOR_BUFFER_BIT,GL_LINEAR);

    glViewport(0,0,hw,hh);
    glActiveTexture(GL_TEXTURE0);

    auto pass=[&](GLuint prog, GLuint src, GLuint dst, float d){
        glBindFramebuffer(GL_FRAMEBUFFER,dst);
        glUseProgram(prog);
        glBindTexture(GL_TEXTURE_2D,src);
        glUniform1i(glGetUniformLocation(prog,"t"),0);
        glUniform1f(glGetUniformLocation(prog,"d"),d);
        quad();
    };

    pass(s_progH, s_tex[0], s_fbo[1], 2.5f/mw);
    pass(s_progV, s_tex[1], s_fbo[2], 2.5f/hh);
    pass(s_progH, s_tex[2], s_fbo[1], 2.5f/hw);
    pass(s_progV, s_tex[1], s_fbo[2], 2.5f/hh);
    pass(s_progH, s_tex[2], s_fbo[1], 2.5f/hw);
    pass(s_progV, s_tex[1], s_fbo[2], 2.5f/hh);
    pass(s_progH, s_tex[2], s_fbo[1], 2.5f/hw);
    pass(s_progV, s_tex[1], s_fbo[2], 2.5f/hh);

    glBindFramebuffer(GL_FRAMEBUFFER,prevFBO);
    glViewport(vp[0],vp[1],vp[2],vp[3]);
    if(blend) glEnable(GL_BLEND);

    s_frozen=true;
}

void Unfreeze(){ s_frozen=false; }

void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float alpha, float rounding){
    if(!s_ready||!s_frozen||alpha<=0.001f) return;
    dl->AddImageRounded(
        (ImTextureID)(intptr_t)s_tex[2],
        p0, p1, {0,0}, {1,1},
        IM_COL32(255,255,255,(int)(alpha*255)),
        rounding
    );
    dl->AddRectFilled(p0, p1, IM_COL32(255,255,255,(int)(alpha*30)), rounding);
}

void Free(){
    for(int i=0;i<3;i++){
        if(s_fbo[i]){glDeleteFramebuffers(1,&s_fbo[i]);s_fbo[i]=0;}
        if(s_tex[i]){glDeleteTextures(1,&s_tex[i]);s_tex[i]=0;}
    }
    if(s_progH){glDeleteProgram(s_progH);s_progH=0;}
    if(s_progV){glDeleteProgram(s_progV);s_progV=0;}
    if(s_vao){glDeleteVertexArrays(1,&s_vao);s_vao=0;}
    if(s_vbo){glDeleteBuffers(1,&s_vbo);s_vbo=0;}
    s_ready=s_frozen=false;
}

}