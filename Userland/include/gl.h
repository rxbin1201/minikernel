#ifndef GL_H
#define GL_H

/* Kleines OpenGL im Stil von Version 1.x (gl.c): Dreiecke zwischen glBegin/glEnd, Matrix-Stapel, Texturen, ein
 * Richtungslicht, Tiefentest. Gezeichnet wird mit der 3D-Pipeline der Intel-GPU direkt in das Fenster des Programms
 * (Systemaufruf SYS_GPUCOMP 8); ohne sie (QEMU, andere Grafik, ausserhalb des Desktops) rechnet dieselbe Bibliothek
 * alles mit der CPU - das Bild ist dasselbe.
 *
 *   gl_open(640, 480, "Titel");          Fenster (Breite wird auf ein Vielfaches von 16 aufgerundet)
 *   for (;;) {
 *       glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
 *       glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0, 0, -5); glRotatef(a, 0, 1, 0);
 *       glBegin(GL_QUADS); glNormal3f(..); glTexCoord2f(..); glVertex3f(..); ... glEnd();
 *       gl_swap();                       anzeigen und auf das naechste Bild des Desktops warten
 *   }
 *
 * Dreiecke werden an der nahen und fernen Ebene abgeschnitten (auch mit einer Ecke hinter der Kamera), Rueckseiten
 * auf Wunsch weggelassen (glEnable(GL_CULL_FACE), glCullFace, glFrontFace). Mischen: glEnable(GL_BLEND),
 * glBlendFunc (alle Faktoren ausser den konstanten, Funktion immer "addieren"), glDepthMask. Durchsichtiges zeichnet
 * man wie ueblich nach allem Deckenden, von hinten nach vorn und mit glDepthMask(GL_FALSE).
 *
 * Einschraenkungen: das Fenster hat keinen Alpha-Kanal (GL_DST_ALPHA ist immer 1), keine Mip-Stufen, ein Licht (GL_LIGHT0, nur Richtung), Texturen RGBA bzw.
 * BGRA mit 8 Bit je Kanal. */

typedef float          GLfloat;
typedef double         GLdouble;
typedef unsigned       GLenum;
typedef unsigned       GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned       GLbitfield;
typedef unsigned char  GLubyte;
typedef unsigned char  GLboolean;
typedef void           GLvoid;
typedef long           GLsizeiptr;
typedef long           GLintptr;

#define GL_FALSE               0
#define GL_TRUE                1
#define GL_DEPTH_BUFFER_BIT    0x0100
#define GL_COLOR_BUFFER_BIT    0x4000
#define GL_TRIANGLES           0x0004
#define GL_TRIANGLE_STRIP      0x0005
#define GL_TRIANGLE_FAN        0x0006
#define GL_QUADS               0x0007
#define GL_QUAD_STRIP          0x0008
#define GL_POLYGON             0x0009
#define GL_FRONT               0x0404
#define GL_BACK                0x0405
#define GL_FRONT_AND_BACK      0x0408
#define GL_CW                  0x0900
#define GL_CCW                 0x0901
#define GL_CULL_FACE           0x0B44
#define GL_LIGHTING            0x0B50
#define GL_BLEND               0x0BE2
#define GL_ZERO                0
#define GL_ONE                 1
#define GL_SRC_COLOR           0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA           0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR           0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE  0x0308
#define GL_DEPTH_TEST          0x0B71
#define GL_TEXTURE_2D          0x0DE1
#define GL_MODELVIEW           0x1700
#define GL_PROJECTION          0x1701
#define GL_UNSIGNED_BYTE       0x1401
#define GL_RGBA                0x1908
#define GL_BGRA                0x80E1
#define GL_NEAREST             0x2600
#define GL_LINEAR              0x2601
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_LIGHT0              0x4000
#define GL_AMBIENT             0x1200
#define GL_DIFFUSE             0x1201
#define GL_POSITION            0x1203
#define GL_BYTE                0x1400
#define GL_UNSIGNED_SHORT      0x1403
#define GL_UNSIGNED_INT        0x1405
#define GL_FLOAT               0x1406
#define GL_VERTEX_ARRAY        0x8074
#define GL_NORMAL_ARRAY        0x8075
#define GL_COLOR_ARRAY         0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_ARRAY_BUFFER        0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW         0x88E4
#define GL_DYNAMIC_DRAW        0x88E8
#define GL_STREAM_DRAW         0x88E0
#define GL_WRITE_ONLY          0x88B9
#define GL_READ_WRITE          0x88BA

/* ---------- Fenster ---------- */
int  gl_open(int w, int h, const char *title); /* 0 = ok */
void gl_close(void);
int  gl_gpu(void);                             /* 1 = die GPU zeichnet, 0 = die CPU */
void gl_force_cpu(int on);                     /* vor gl_open: immer mit der CPU zeichnen (zum Vergleich) */
void gl_swap(void);                            /* Bild fertig: anzeigen, auf das naechste Bild des Desktops warten */
int  gl_width(void);
int  gl_height(void);

/* ---------- OpenGL ---------- */
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClearDepth(GLdouble d);
void glClear(GLbitfield mask);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glCullFace(GLenum mode);                  /* GL_BACK (Standard), GL_FRONT, GL_FRONT_AND_BACK */
void glFrontFace(GLenum mode);                 /* GL_CCW (Standard): gegen den Uhrzeigersinn ist vorn */
void glBlendFunc(GLenum sfactor, GLenum dfactor); /* Standard GL_ONE, GL_ZERO */
void glDepthMask(GLboolean flag);              /* GL_FALSE: Tiefentest weiter, aber Tiefe nicht schreiben */
void glFlush(void);
void glFinish(void);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);          /* spaltenweise wie OpenGL */
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat deg, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble n, GLdouble f);

void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glTexCoord2f(GLfloat u, GLfloat v);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3fv(const GLfloat *n);

void glGenTextures(GLsizei n, GLuint *ids);
void glDeleteTextures(GLsizei n, const GLuint *ids);
void glBindTexture(GLenum target, GLuint id);
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);

void glLightfv(GLenum light, GLenum pname, const GLfloat *v);

/* Puffer (OpenGL 1.5) und Vertex-Arrays: Ecken und Indizes bleiben im Speicher (fuer die GPU angemeldet), gezeichnet
 * wird mit glDrawArrays/glDrawElements. Arrays: Positionen (2-4 float), Textur (1-4 float), Normalen (3 float),
 * Farben (3-4 float oder 4 GL_UNSIGNED_BYTE); mit gebundenem GL_ARRAY_BUFFER ist der Zeiger ein Abstand darin. */
void  glGenBuffers(GLsizei n, GLuint *ids);
void  glDeleteBuffers(GLsizei n, const GLuint *ids);
void  glBindBuffer(GLenum target, GLuint id);   /* GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER; 0 = keiner */
void  glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void  glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data);
void *glMapBuffer(GLenum target, GLenum access);
GLboolean glUnmapBuffer(GLenum target);
void  glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void  glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void  glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p);
void  glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void  glEnableClientState(GLenum cap);         /* GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY, GL_NORMAL_ARRAY, ... */
void  glDisableClientState(GLenum cap);
void  glDrawArrays(GLenum mode, GLint first, GLsizei count);
void  glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices); /* GL_UNSIGNED_BYTE/SHORT/INT */

/* Rechenhilfen (die libc hat keine Mathefunktionen) */
float gl_sinf(float x);
float gl_cosf(float x);
float gl_sqrtf(float x);

#endif
