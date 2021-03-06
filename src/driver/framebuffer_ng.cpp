/*
	Neutrino-GUI  -   DBoxII-Project

	Copyright (C) 2001 Steffen Hehn 'McClean'
                      2003 thegoodguy
	Copyright (C) 2007-2013 Stefan Seyfried

	License: GPL

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS 64
#include <png.h>
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <driver/framebuffer_ng.h>

#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <memory.h>
#include <math.h>

#include <linux/kd.h>

#ifdef USE_OPENGL
#include <glfb.h>
extern GLFramebuffer *glfb;
#endif

#include <gui/audiomute.h>
#include <gui/color.h>
#include <gui/pictureviewer.h>
#include <global.h>
#include <video.h>
#include <cs_api.h>
#ifdef HAVE_COOL_HARDWARE
#include <cnxtfb.h>
#endif
#if HAVE_TRIPLEDRAGON
#include <tdgfx/stb04gfx.h>
extern int gfxfd;
#endif
#include <system/set_threadname.h>

extern CPictureViewer * g_PicViewer;
#define ICON_CACHE_SIZE 1024*1024*2 // 2mb

#define BACKGROUNDIMAGEWIDTH 720

/* note that it is *not* enough to just change those values */
#define DEFAULT_XRES 1280
#define DEFAULT_YRES 720
#define DEFAULT_BPP  32

/*******************************************************************************/

void CFrameBuffer::waitForIdle(const char *)
{
	accel->waitForIdle();
}

/*******************************************************************************/

static uint8_t * virtual_fb = NULL;
inline unsigned int make16color(uint16_t r, uint16_t g, uint16_t b, uint16_t t,
                                  uint32_t  /*rl*/ = 0, uint32_t  /*ro*/ = 0,
                                  uint32_t  /*gl*/ = 0, uint32_t  /*go*/ = 0,
                                  uint32_t  /*bl*/ = 0, uint32_t  /*bo*/ = 0,
                                  uint32_t  /*tl*/ = 0, uint32_t  /*to*/ = 0)
{
        return ((t << 24) & 0xFF000000) | ((r << 8) & 0xFF0000) | ((g << 0) & 0xFF00) | (b >> 8 & 0xFF);
}

// return bottom right corner as alpha bitmap
static unsigned char *getQuarterCircle(unsigned int radius, bool filled = true)
{
	if (radius < 2)
		return NULL;
	int key = filled ? -radius : radius;

	static std::map<int,unsigned char *> qa_map;
	std::map<int, unsigned char *>::iterator it = qa_map.find(key);
	if (it != qa_map.end())
		return (*it).second;

	unsigned char *qa = (unsigned char *) calloc(1, radius * radius);
	if (!qa)
		return NULL;
	qa_map[key] = qa;

	unsigned int cs = radius;
	radius--;

	unsigned int r2 = radius * radius;
	unsigned int cs2 = cs * cs;
	for (unsigned int x = 0; x < radius; x++) {
		double yr = sqrt(r2 - x*x);
		unsigned int y = (unsigned int) floor(yr);
		unsigned char a1 = (0xff) & (unsigned char)(256.0 * (yr - y));
		unsigned char a0 = 255 - a1;
		qa[x + cs * y] = (unsigned char)a0;
		qa[y + cs * x] = a0;
		unsigned int g = x + cs * (y + 1);
		if (g < cs2)
			qa[g] = a1;
		g = y + 1 + cs * x;
		if (g < cs2)
			qa[g] = a1;
		if (x >= y)
			break;
	}
	if (filled)
		for (unsigned int y = 0; y < radius; y++) {
			unsigned char *p = qa + y * cs;
			unsigned int x = 0;
			for (; x < cs && !*p; x++, p++)
				*p = 255;
			for (; x < radius && *(p + 1); x++, p++)
				*p = 255;
		}
	return qa;
}

static unsigned char *getQuarterArc(unsigned int rad_outer, unsigned int rad_inner)
{
	rad_inner++;
        unsigned int key = (rad_outer << 16 | rad_inner);

        static std::map<unsigned int,unsigned char *> qa_map;
        std::map<unsigned int, unsigned char *>::iterator it = qa_map.find(key);
        if (it != qa_map.end())
                return (*it).second;

        unsigned char *innerQuarter = getQuarterCircle(rad_inner, false);
        unsigned char *outerQuarter = getQuarterCircle(rad_outer);
        if (!innerQuarter || !outerQuarter)
                return NULL;

        unsigned char *qa = (unsigned char *) calloc(1, rad_outer * rad_outer);
        if (!qa)
                return NULL;
        qa_map[key] = qa;

        memcpy(qa, outerQuarter, rad_outer * rad_outer);

        for (unsigned int y = 0; y < rad_inner; y++) {
		unsigned x = 0;
                for (; x < rad_inner && !innerQuarter[y * rad_inner + x]; x++)
			qa[y * rad_outer + x] = qa[x * rad_outer + y] = innerQuarter[y * rad_inner + x];
                if (x < rad_inner && innerQuarter[y * rad_inner + x])
			qa[y * rad_outer + x] = qa[x * rad_outer + y] = innerQuarter[y * rad_inner + x];
		if (x <= y)
			break;
	}

	return qa;
}


static inline int limitRadius(const int& dx, const int& dy, const int& radius)
{
	int m = std::min(dx, dy);
	return std::min(m, radius);
}

CFrameBuffer::CFrameBuffer()
: active ( true )
{
	iconBasePath = "";
	available  = 0;
	cmap.start = 0;
	cmap.len = 256;
	cmap.red = red;
	cmap.green = green;
	cmap.blue  = blue;
	cmap.transp = trans;
	backgroundColor = 0;
	useBackgroundPaint = false;
	background = NULL;
	backupBackground = NULL;
	backgroundFilename = "";
	fbAreaActiv = false;
	fb_no_check = false;
	fd  = 0;
	tty = 0;
	bpp = 0;
	locked = false;
	m_transparent_default = CFrameBuffer::TM_BLACK; // TM_BLACK: Transparency when black content ('pseudo' transparency)
							// TM_NONE:  No 'pseudo' transparency
							// TM_INI:   Transparency depends on g_settings.infobar_alpha ???
	m_transparent         = m_transparent_default;
//FIXME: test
	memset(red, 0, 256*sizeof(__u16));
	memset(green, 0, 256*sizeof(__u16));
	memset(blue, 0, 256*sizeof(__u16));
	memset(trans, 0, 256*sizeof(__u16));
#if HAVE_SPARK_HARDWARE
	autoBlitStatus = false;
	autoBlitThreadId = 0;
#endif
}

CFrameBuffer* CFrameBuffer::getInstance()
{
	static CFrameBuffer* frameBuffer = NULL;

	if(!frameBuffer) {
		frameBuffer = new CFrameBuffer();
		printf("[neutrino] frameBuffer Instance created\n");
	} else {
		//printf("[neutrino] frameBuffer Instace requested\n");
	}
	return frameBuffer;
}

#ifdef USE_NEVIS_GXA
void CFrameBuffer::setupGXA(void)
{
	accel->setupGXA();
}
#endif

void CFrameBuffer::init(const char * const fbDevice)
{
        int tr = 0xFF;
#ifdef USE_OPENGL
	(void)fbDevice;
	fd = -1;
	if (!glfb) {
		fprintf(stderr, "CFrameBuffer::init: GL Framebuffer is not set up? we are doomed...\n");
		goto nolfb;
	}
	screeninfo = glfb->getScreenInfo();
	stride = 4 * screeninfo.xres;
	available = glfb->getOSDBuffer()->size(); /* allocated in glfb constructor */
	lfb = reinterpret_cast<fb_pixel_t*>(glfb->getOSDBuffer()->data());
	memset(lfb, 0x7f, stride * screeninfo.yres);
#else
	fd = open(fbDevice, O_RDWR);
	if(!fd) fd = open(fbDevice, O_RDWR);

	if (fd<0) {
		perror(fbDevice);
		goto nolfb;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &screeninfo)<0) {
		perror("FBIOGET_VSCREENINFO");
		goto nolfb;
	}

	memmove(&oldscreen, &screeninfo, sizeof(screeninfo));

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix)<0) {
		perror("FBIOGET_FSCREENINFO");
		goto nolfb;
	}

	available=fix.smem_len;
	printf("%dk video mem\n", available/1024);
	lfb=(fb_pixel_t*)mmap(0, available, PROT_WRITE|PROT_READ, MAP_SHARED, fd, 0);

	if (lfb == MAP_FAILED) {
		perror("mmap");
		goto nolfb;
	}

	memset(lfb, 0, available);
#endif /* USE_OPENGL */
	cache_size = 0;

        /* Windows Colors */
        paletteSetColor(0x1, 0x010101, tr);
        paletteSetColor(0x2, 0x800000, tr);
        paletteSetColor(0x3, 0x008000, tr);
        paletteSetColor(0x4, 0x808000, tr);
        paletteSetColor(0x5, 0x000080, tr);
        paletteSetColor(0x6, 0x800080, tr);
        paletteSetColor(0x7, 0x008080, tr);
        paletteSetColor(0x8, 0xA0A0A0, tr);
        paletteSetColor(0x9, 0x505050, tr);
        paletteSetColor(0xA, 0xFF0000, tr);
        paletteSetColor(0xB, 0x00FF00, tr);
        paletteSetColor(0xC, 0xFFFF00, tr);
        paletteSetColor(0xD, 0x0000FF, tr);
        paletteSetColor(0xE, 0xFF00FF, tr);
        paletteSetColor(0xF, 0x00FFFF, tr);
        paletteSetColor(0x10, 0xFFFFFF, tr);
        paletteSetColor(0x11, 0x000000, tr);
        paletteSetColor(COL_BACKGROUND, 0x000000, 0xffff);

        paletteSet();

        useBackground(false);
	m_transparent = m_transparent_default;
	accel = new CFbAccel(this);
#if HAVE_SPARK_HARDWARE
	setMixerColor(g_settings.video_mixer_color);
#endif
	return;

nolfb:
	printf("framebuffer not available.\n");
	lfb=0;
}


CFrameBuffer::~CFrameBuffer()
{
#if HAVE_SPARK_HARDWARE
	autoBlit(false);
#endif
	active = false; /* keep people/infoclocks from accessing */
	std::map<std::string, rawIcon>::iterator it;

	for(it = icon_cache.begin(); it != icon_cache.end(); ++it) {
		/* printf("FB: delete cached icon %s: %x\n", it->first.c_str(), (int) it->second.data); */
		cs_free_uncached(it->second.data);
	}
	icon_cache.clear();

	if (background) {
		delete[] background;
		background = NULL;
	}

	if (backupBackground) {
		delete[] backupBackground;
		backupBackground = NULL;
	}

	if (lfb)
		munmap(lfb, available);

	if (virtual_fb){
		delete[] virtual_fb;
		virtual_fb = NULL;
	}
	delete accel;
	close(fd);
}

int CFrameBuffer::getFileHandle() const
{
	return fd;
}

unsigned int CFrameBuffer::getStride() const
{
	return stride;
}

#if HAVE_SPARK_HARDWARE
unsigned int CFrameBuffer::getScreenWidth(bool)
{
	return DEFAULT_XRES;
}

unsigned int CFrameBuffer::getScreenHeight(bool)
{
	return DEFAULT_YRES;
}

unsigned int CFrameBuffer::getScreenWidthRel(bool force_small)
{
	int percent = force_small ? WINDOW_SIZE_MIN_FORCED : g_settings.window_width;
	// always reduce a possible detailline
	return (DEFAULT_XRES - 2*ConnectLineBox_Width) * percent / 100;
}

unsigned int CFrameBuffer::getScreenHeightRel(bool force_small)
{
	int percent = force_small ? WINDOW_SIZE_MIN_FORCED : g_settings.window_height;
	return DEFAULT_YRES * percent / 100;
}

unsigned int CFrameBuffer::getScreenX()
{
	return 0;
}

unsigned int CFrameBuffer::getScreenY()
{
	return 0;
}
#else
unsigned int CFrameBuffer::getScreenWidth(bool real)
{
	if(real)
		return xRes;
	else
		return g_settings.screen_EndX - g_settings.screen_StartX;
}

unsigned int CFrameBuffer::getScreenHeight(bool real)
{
	if(real)
		return yRes;
	else
		return g_settings.screen_EndY - g_settings.screen_StartY;
}

unsigned int CFrameBuffer::getScreenWidthRel(bool force_small)
{
	int percent = force_small ? WINDOW_SIZE_MIN_FORCED : g_settings.window_width;
	// always reduce a possible detailline
	return (g_settings.screen_EndX - g_settings.screen_StartX - 2*ConnectLineBox_Width) * percent / 100;
}

unsigned int CFrameBuffer::getScreenHeightRel(bool force_small)
{
	int percent = force_small ? WINDOW_SIZE_MIN_FORCED : g_settings.window_height;
	return (g_settings.screen_EndY - g_settings.screen_StartY) * percent / 100;
}

unsigned int CFrameBuffer::getScreenX()
{
	return g_settings.screen_StartX;
}

unsigned int CFrameBuffer::getScreenY()
{
	return g_settings.screen_StartY;
}
#endif

fb_pixel_t * CFrameBuffer::getFrameBufferPointer(bool real)
{
	if (real)
		return lfb;
	if (active || (virtual_fb == NULL))
		return accel->lbb;
	else
		return (fb_pixel_t *)virtual_fb;
}

fb_pixel_t * CFrameBuffer::getBackBufferPointer() const
{
	return accel->backbuffer;
}

bool CFrameBuffer::getActive() const
{
	return (active || (virtual_fb != NULL));
}

void CFrameBuffer::setActive(bool enable)
{
	active = enable;
}

t_fb_var_screeninfo *CFrameBuffer::getScreenInfo()
{
	return &screeninfo;
}

int CFrameBuffer::setMode(unsigned int /*nxRes*/, unsigned int /*nyRes*/, unsigned int /*nbpp*/)
{
fprintf(stderr, "CFrameBuffer::setMode avail: %d active: %d\n", available, active);
	if (!available&&!active)
		return -1;

#if HAVE_AZBOX_HARDWARE
#ifndef FBIO_BLIT
#define FBIO_SET_MANUAL_BLIT _IOW('F', 0x21, __u8)
#define FBIO_BLIT 0x22
#endif
	// set manual blit if AZBOX_MANUAL_BLIT environment variable is set
	unsigned char tmp = getenv("AZBOX_MANUAL_BLIT") ? 1 : 0;
	if (ioctl(fd, FBIO_SET_MANUAL_BLIT, &tmp)<0)
		perror("FBIO_SET_MANUAL_BLIT");

	const unsigned int nxRes = DEFAULT_XRES;
	const unsigned int nyRes = DEFAULT_YRES;
	const unsigned int nbpp  = DEFAULT_BPP;
	screeninfo.xres_virtual = screeninfo.xres = nxRes;
	screeninfo.yres_virtual = (screeninfo.yres = nyRes) * 2;
	screeninfo.height = 0;
	screeninfo.width = 0;
	screeninfo.xoffset = screeninfo.yoffset = 0;
	screeninfo.bits_per_pixel = nbpp;

	screeninfo.transp.offset = 24;
	screeninfo.transp.length = 8;
	screeninfo.red.offset = 16;
	screeninfo.red.length = 8;
	screeninfo.green.offset = 8;
	screeninfo.green.length = 8;
	screeninfo.blue.offset = 0;
	screeninfo.blue.length = 8;

	if (ioctl(fd, FBIOPUT_VSCREENINFO, &screeninfo)<0) {
		// try single buffering
		screeninfo.yres_virtual = screeninfo.yres = nyRes;
		if (ioctl(fd, FBIOPUT_VSCREENINFO, &screeninfo) < 0)
		perror("FBIOPUT_VSCREENINFO");
		printf("FB: double buffering not available.\n");
	}
	else
		printf("FB: double buffering available!\n");

	ioctl(fd, FBIOGET_VSCREENINFO, &screeninfo);

	if ((screeninfo.xres!=nxRes) && (screeninfo.yres!=nyRes) && (screeninfo.bits_per_pixel!=nbpp))
	{
		printf("SetMode failed: wanted: %dx%dx%d, got %dx%dx%d\n",
		       nxRes, nyRes, nbpp,
		       screeninfo.xres, screeninfo.yres, screeninfo.bits_per_pixel);
	}
#endif
#if HAVE_SPARK_HARDWARE
	/* it's all fake... :-) */
	screeninfo.xres = screeninfo.xres_virtual = DEFAULT_XRES;
	screeninfo.yres = screeninfo.yres_virtual = DEFAULT_YRES;
	screeninfo.bits_per_pixel = DEFAULT_BPP;
	stride = screeninfo.xres * screeninfo.bits_per_pixel / 8;
#else
#ifndef USE_OPENGL
	fb_fix_screeninfo _fix;

	if (ioctl(fd, FBIOGET_FSCREENINFO, &_fix)<0) {
		perror("FBIOGET_FSCREENINFO");
		return -1;
	}
	stride = _fix.line_length;
#endif
#endif
	xRes = screeninfo.xres;
	yRes = screeninfo.yres;
	bpp  = screeninfo.bits_per_pixel;
	printf("FB: %dx%dx%d line length %d. %s nevis GXA accelerator.\n", xRes, yRes, bpp, stride,
#ifdef USE_NEVIS_GXA
		"Using"
#else
		"Not using"
#endif
	);
	accel->update(); /* just in case we need to update stuff */

	//memset(getFrameBufferPointer(), 0, stride * yRes);
	paintBackground();
#if HAVE_COOL_HARDWARE
        if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
                printf("screen unblanking failed\n");
        }
#endif
	return 0;
}
#if 0 
//never used
void CFrameBuffer::setTransparency( int /*tr*/ )
{
}
#endif

#if HAVE_SPARK_HARDWARE
/* original interfaceL: 1 == pixel alpha, 2 == global alpha premultiplied */
void CFrameBuffer::setBlendMode(uint8_t mode)
{
	/* mode = 1 => reset to no extra transparency */
	if (mode == 1)
		setBlendLevel(0);
}

/* level = 100 -> transparent, level = 0 -> nontransperent */
void CFrameBuffer::setBlendLevel(int level)
{
	struct stmfbio_var_screeninfo_ex v;
	memset(&v, 0, sizeof(v));
	/* set to 0 already...
	 v.layerid = 0;
	 v.activate = STMFBIO_ACTIVATE_IMMEDIATE; // == 0
	 v.premultiplied_alpha = 0;
	*/
	v.caps = STMFBIO_VAR_CAPS_OPACITY | STMFBIO_VAR_CAPS_PREMULTIPLIED;
	v.opacity = 0xff - (level * 0xff / 100);
	if (ioctl(fd, STMFBIO_SET_VAR_SCREENINFO_EX, &v) < 0)
		perror("[fb:setBlendLevel] STMFBIO");
}
#elif !HAVE_TRIPLEDRAGON
void CFrameBuffer::setBlendMode(uint8_t mode)
{
	(void)mode;
#ifdef HAVE_COOL_HARDWARE
	if (ioctl(fd, FBIO_SETBLENDMODE, mode))
		printf("FBIO_SETBLENDMODE failed.\n");
#endif
}

void CFrameBuffer::setBlendLevel(int level)
{
	(void)level;
#ifdef HAVE_COOL_HARDWARE
	//printf("CFrameBuffer::setBlendLevel %d\n", level);
	unsigned char value = 0xFF;
	if((level >= 0) && (level <= 100))
		value = convertSetupAlpha2Alpha(level);

	if (ioctl(fd, FBIO_SETOPACITY, value))
		printf("FBIO_SETOPACITY failed.\n");
#if 1
       if(level == 100) // TODO: sucks.
               usleep(20000);
#endif
#endif
}
#else
/* TRIPLEDRAGON */
void CFrameBuffer::setBlendMode(uint8_t mode)
{
	Stb04GFXOsdControl g;
	ioctl(gfxfd, STB04GFX_OSD_GETCONTROL, &g);
	g.use_global_alpha = (mode == 2); /* 1 == pixel alpha, 2 == global alpha */
	ioctl(gfxfd, STB04GFX_OSD_SETCONTROL, &g);
}

void CFrameBuffer::setBlendLevel(int level)
{
	/* this is bypassing directfb, but faster and easier */
	Stb04GFXOsdControl g;
	ioctl(gfxfd, STB04GFX_OSD_GETCONTROL, &g);
	if (g.use_global_alpha == 0)
		return;

	if (level < 0 || level > 100)
		return;

	/* this is the same as convertSetupAlpha2Alpha(), but non-float */
	g.global_alpha = 255 - (255 * level / 100);
	ioctl(gfxfd, STB04GFX_OSD_SETCONTROL, &g);
	if (level == 100) // sucks
		usleep(20000);
}
#endif

#if 0 
//never used
void CFrameBuffer::setAlphaFade(int in, int num, int tr)
{
	for (int i=0; i<num; i++) {
		cmap.transp[in+i]=tr;
	}
}
#endif
void CFrameBuffer::paletteFade(int i, __u32 rgb1, __u32 rgb2, int level)
{
	__u16 *r = cmap.red+i;
	__u16 *g = cmap.green+i;
	__u16 *b = cmap.blue+i;
	*r= ((rgb2&0xFF0000)>>16)*level;
	*g= ((rgb2&0x00FF00)>>8 )*level;
	*b= ((rgb2&0x0000FF)    )*level;
	*r+=((rgb1&0xFF0000)>>16)*(255-level);
	*g+=((rgb1&0x00FF00)>>8 )*(255-level);
	*b+=((rgb1&0x0000FF)    )*(255-level);
}

void CFrameBuffer::paletteGenFade(int in, __u32 rgb1, __u32 rgb2, int num, int tr)
{
	for (int i=0; i<num; i++) {
		paletteFade(in+i, rgb1, rgb2, i*(255/(num-1)));
		cmap.transp[in+i]=tr;
		tr--; //FIXME
	}
}

void CFrameBuffer::paletteSetColor(int i, __u32 rgb, int tr)
{
	cmap.red[i]    =(rgb&0xFF0000)>>8;
	cmap.green[i]  =(rgb&0x00FF00)   ;
	cmap.blue[i]   =(rgb&0x0000FF)<<8;
	cmap.transp[i] = tr;
}

void CFrameBuffer::paletteSet(struct fb_cmap *map)
{
	if (!active)
		return;

	if(map == NULL)
		map = &cmap;

	if(bpp == 8) {
//printf("Set palette for %dbit\n", bpp);
		ioctl(fd, FBIOPUTCMAP, map);
	}
	uint32_t  rl, ro, gl, go, bl, bo, tl, to;
        rl = screeninfo.red.length;
        ro = screeninfo.red.offset;
        gl = screeninfo.green.length;
        go = screeninfo.green.offset;
        bl = screeninfo.blue.length;
        bo = screeninfo.blue.offset;
        tl = screeninfo.transp.length;
        to = screeninfo.transp.offset;
	for (int i = 0; i < 256; i++) {
                realcolor[i] = make16color(cmap.red[i], cmap.green[i], cmap.blue[i], cmap.transp[i],
                                           rl, ro, gl, go, bl, bo, tl, to);
	}
	realcolor[(COL_BACKGROUND + 0)] = 0; // background, no alpha
}

inline fb_pixel_t mergeColor(fb_pixel_t oc, int ol, fb_pixel_t ic, int il)
{
	return   ((( (oc >> 24)         * ol) + ( (ic >> 24)         * il)) >> 8) << 24
		|(((((oc >> 16) & 0xff) * ol) + (((ic >> 16) & 0xff) * il)) >> 8) << 16
		|(((((oc >> 8)  & 0xff) * ol) + (((ic >>  8) & 0xff) * il)) >> 8) << 8
		|(((( oc        & 0xff) * ol) + (( ic        & 0xff) * il)) >> 8);
}

void CFrameBuffer::paintBoxRel(const int x, const int y, const int dx, const int dy, const fb_pixel_t col, int radius, int type)
{
	/* draw a filled rectangle (with additional round corners) */
	if (!getActive())
		return;

	checkFbArea(x, y, dx, dy, true);

#if HAVE_SPARK_HARDWARE
	// hack: don't paint round corners unless these are actually visible --martii
	fb_pixel_t *f = accel->lbb + y * stride/sizeof(fb_pixel_t) + x;
	if ((col == *f)
	 && (col == *(f + dx - 1))
	 && (col == *(f + (dy - 1) * stride/sizeof(fb_pixel_t)))
	 && (col == *(f + (dy - 1) * stride/sizeof(fb_pixel_t) + dx - 1)))
		type = 0;
#endif

	unsigned char *corner = NULL;
	if (type && radius) {
		/* limit the radius */
		radius = limitRadius(dx, dy, radius);
		if (radius < 1)		/* dx or dy = 0... */
			radius = 1;	/* avoid div by zero below */

		corner = getQuarterCircle(radius);
	}

	if (!corner)
	{
		accel->paintRect(x, y, dx, dy, col);
		checkFbArea(x, y, dx, dy, false);
		return;
	}

	accel->waitForIdle();

	int radius_tl = (type & CORNER_TOP_LEFT) ? radius : 0;
	int radius_tr = (type & CORNER_TOP_RIGHT) ? radius : 0;
	int radius_bl = (type & CORNER_BOTTOM_LEFT) ? radius : 0;
	int radius_br = (type & CORNER_BOTTOM_RIGHT) ? radius : 0;

	int radius_t = (type & (CORNER_TOP_LEFT | CORNER_TOP_RIGHT)) ? radius : 0;
	int radius_b = (type & (CORNER_BOTTOM_LEFT | CORNER_BOTTOM_RIGHT)) ? radius : 0;

	if (radius_tl) {
		fb_pixel_t *p = accel->lbb + y * stride/sizeof(fb_pixel_t) + x;
		for (int cy = radius - 1; cy > -1; cy--) {
			for (int cx = radius - 1; cx > -1; cx--, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}
	if (radius_tr) {
		fb_pixel_t *p = accel->lbb + y * stride/sizeof(fb_pixel_t) + x + dx - radius_tr;
		for (int cy = radius - 1; cy > -1; cy--) {
			for (int cx = 0; cx < radius; cx++, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}
	if (radius_bl) {
		fb_pixel_t *p = accel->lbb + (y + dy - radius_b) * stride/sizeof(fb_pixel_t) + x;
		for (int cy = 0; cy < radius; cy++) {
			for (int cx = radius - 1; cx > -1; cx--, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}
	if (radius_br) {
		fb_pixel_t *p = accel->lbb + (y + dy - radius_b) * stride/sizeof(fb_pixel_t) + x + dx - radius_br;
		for (int cy = 0; cy < radius; cy++) {
			for (int cx = 0; cx < radius; cx++, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}

	if (radius_t)
		accel->paintRect(x + radius_tl, y                , dx - radius_tl - radius_tr, radius_t, col); // top
	if (radius_b)
		accel->paintRect(x + radius_bl, y + dy - radius_b, dx - radius_bl - radius_br, radius_b, col); // bottom
	accel->paintRect(x, y + radius_t, dx, dy - radius_t - radius_b, col);

	checkFbArea(x, y, dx, dy, false);
}

void CFrameBuffer::paintPixel(const int x, const int y, const fb_pixel_t col)
{
	if (!getActive())
		return;
	if (x > (int)xRes || y > (int)yRes || x < 0 || y < 0)
		return;

	accel->paintPixel(x, y, col);
}

void CFrameBuffer::paintLine(int xa, int ya, int xb, int yb, const fb_pixel_t col)
{
	if (!getActive())
		return;

	accel->paintLine(xa, ya, xb, yb, col);
}

void CFrameBuffer::paintVLine(int x, int ya, int yb, const fb_pixel_t col)
{
	paintLine(x, ya, x, yb, col);
}

void CFrameBuffer::paintVLineRel(int x, int y, int dy, const fb_pixel_t col)
{
	paintLine(x, y, x, y + dy, col);
}

void CFrameBuffer::paintHLine(int xa, int xb, int y, const fb_pixel_t col)
{
	paintLine(xa, y, xb, y, col);
}

void CFrameBuffer::paintHLineRel(int x, int dx, int y, const fb_pixel_t col)
{
	paintLine(x, y, x + dx, y, col);
}

void CFrameBuffer::setIconBasePath(const std::string & iconPath)
{
	iconBasePath = iconPath;
}

void CFrameBuffer::getIconSize(const char * const filename, int* width, int *height)
{
	*width = 0;
	*height = 0;

	if(filename == NULL)
		return;

	std::map<std::string, rawIcon>::iterator it;

	/* if code ask for size, lets cache it. assume we have enough ram for cache */
	/* FIXME offset seems never used in code, always default = 1 ? */

	it = icon_cache.find(filename);
	if(it == icon_cache.end()) {
		if(paintIcon(filename, 0, 0, 0, 1, false)) {
			it = icon_cache.find(filename);
		}
	}
	if(it != icon_cache.end()) {
		*width = it->second.width;
		*height = it->second.height;
	}
}

bool CFrameBuffer::paintIcon8(const std::string & filename, const int x, const int y, const unsigned char offset)
{
	if (!getActive())
		return false;

//printf("%s(file, %d, %d, %d)\n", __FUNCTION__, x, y, offset);

	struct rawHeader header;
	uint16_t         width, height;
	int              lfd;

	lfd = open((iconBasePath + filename).c_str(), O_RDONLY);

	if (lfd == -1) {
		printf("paintIcon8: error while loading icon: %s%s\n", iconBasePath.c_str(), filename.c_str());
		return false;
	}

	read(lfd, &header, sizeof(struct rawHeader));

	width  = (header.width_hi  << 8) | header.width_lo;
	height = (header.height_hi << 8) | header.height_lo;

	unsigned char pixbuf[768];

	uint8_t * d = ((uint8_t *)getFrameBufferPointer()) + x * sizeof(fb_pixel_t) + stride * y;
	fb_pixel_t * d2;
	for (int count=0; count<height; count ++ ) {
		read(lfd, &pixbuf[0], width );
		unsigned char *pixpos = &pixbuf[0];
		d2 = (fb_pixel_t *) d;
		for (int count2=0; count2<width; count2 ++ ) {
			unsigned char color = *pixpos;
			if (color != header.transp) {
//printf("icon8: col %d transp %d real %08X\n", color+offset, header.transp, realcolor[color+offset]);
				paintPixel(d2, color + offset);
			}
			d2++;
			pixpos++;
		}
		d += stride;
	}
	close(lfd);
	blit();
	return true;
}

/* paint icon at position x/y,
   if height h is given, center vertically between y and y+h
   offset is a color offset (probably only useful with palette) */
bool CFrameBuffer::paintIcon(const std::string & filename, const int x, const int y,
			     const int h, const unsigned char offset, bool paint, bool paintBg, const fb_pixel_t colBg)
{
	struct rawHeader header;
	int         width, height;
	int              lfd;
	fb_pixel_t * data;
	struct rawIcon tmpIcon;
	std::map<std::string, rawIcon>::iterator it;
	int dsize;

	if (!getActive())
		return false;

	int  yy = y;
	//printf("CFrameBuffer::paintIcon: load %s\n", filename.c_str());fflush(stdout);

	/* we cache and check original name */
	it = icon_cache.find(filename);
	if(it == icon_cache.end()) {
		std::string newname = iconBasePath + filename.c_str() + ".png";
		//printf("CFrameBuffer::paintIcon: check for %s\n", newname.c_str());fflush(stdout);

		data = g_PicViewer->getIcon(newname, &width, &height);

		if(data) {
			dsize = width*height*sizeof(fb_pixel_t);
			//printf("CFrameBuffer::paintIcon: %s found, data %x size %d x %d\n", newname.c_str(), data, width, height);fflush(stdout);
			if(cache_size+dsize >= ICON_CACHE_SIZE) {
				//purge cache
				for(it = icon_cache.begin(); it != icon_cache.end(); ++it)
					cs_free_uncached(it->second.data);
				icon_cache.clear();
				cache_size = 0;
			}
			if(cache_size+dsize < ICON_CACHE_SIZE) {
				cache_size += dsize;
				tmpIcon.width = width;
				tmpIcon.height = height;
				tmpIcon.data = data;
				icon_cache.insert(std::pair <std::string, rawIcon> (filename, tmpIcon));
				//printf("Cached %s, cache size %d\n", newname.c_str(), cache_size);
			}
			goto _display;
		}

		newname = iconBasePath + filename.c_str() + ".raw";

		lfd = open(newname.c_str(), O_RDONLY);

		if (lfd == -1) {
			//printf("paintIcon: error while loading icon: %s\n", newname.c_str());
			return false;
		}
		read(lfd, &header, sizeof(struct rawHeader));

		tmpIcon.width = width  = (header.width_hi  << 8) | header.width_lo;
		tmpIcon.height = height = (header.height_hi << 8) | header.height_lo;

		dsize = width*height*sizeof(fb_pixel_t);

		tmpIcon.data = (fb_pixel_t*) cs_malloc_uncached(dsize);
		data = tmpIcon.data;

		unsigned char pixbuf[768];
		for (int count = 0; count < height; count ++ ) {
			read(lfd, &pixbuf[0], width >> 1 );
			unsigned char *pixpos = &pixbuf[0];
			for (int count2 = 0; count2 < width >> 1; count2 ++ ) {
				unsigned char compressed = *pixpos;
				unsigned char pix1 = (compressed & 0xf0) >> 4;
				unsigned char pix2 = (compressed & 0x0f);
				if (pix1 != header.transp)
					*data++ = realcolor[pix1+offset];
				else
					*data++ = 0;
				if (pix2 != header.transp)
					*data++ = realcolor[pix2+offset];
				else
					*data++ = 0;
				pixpos++;
			}
		}
		close(lfd);

		data = tmpIcon.data;

		if(cache_size+dsize < ICON_CACHE_SIZE) {
			cache_size += dsize;
			icon_cache.insert(std::pair <std::string, rawIcon> (filename, tmpIcon));
			//printf("Cached %s, cache size %d\n", newname.c_str(), cache_size);
		}
	} else {
		data = it->second.data;
		width = it->second.width;
		height = it->second.height;
		//printf("paintIcon: already cached %s %d x %d\n", newname.c_str(), width, height);
	}
_display:
	if(!paint)
		return true;

	if (h != 0)
		yy += (h - height) / 2;

	checkFbArea(x, yy, width, height, true);
	if (paintBg)
		paintBoxRel(x, yy, width, height, colBg);
	blit2FB(data, width, height, x, yy);
	checkFbArea(x, yy, width, height, false);
	return true;
 
}

void CFrameBuffer::loadPal(const std::string & filename, const unsigned char offset, const unsigned char endidx)
{
	if (!getActive())
		return;

//printf("%s()\n", __FUNCTION__);

	struct rgbData rgbdata;
	int            lfd;

	lfd = open((iconBasePath + filename).c_str(), O_RDONLY);

	if (lfd == -1) {
		printf("error while loading palette: %s%s\n", iconBasePath.c_str(), filename.c_str());
		return;
	}

	int pos = 0;
	int readb = read(lfd, &rgbdata,  sizeof(rgbdata) );
	while(readb) {
		__u32 rgb = (rgbdata.r<<16) | (rgbdata.g<<8) | (rgbdata.b);
		int colpos = offset+pos;
		if( colpos>endidx)
			break;

		paletteSetColor(colpos, rgb, 0xFF);
		readb = read(lfd, &rgbdata,  sizeof(rgbdata) );
		pos++;
	}
	paletteSet(&cmap);
	close(lfd);
}


void CFrameBuffer::paintBoxFrame(const int x, const int y, const int dx, const int dy, const int px, const fb_pixel_t col, const int rad, int type)
{
	if (!getActive())
		return;

	int radius = rad;

	radius = limitRadius(dx, dy, rad);
	if (radius - px < 1)		/* dx or dy = 0... */
		radius = px + 1;	/* avoid div by zero below */

	unsigned char *corner = NULL;
	if (radius && type)
		corner = getQuarterArc(radius, radius - px);

	int radius_tl = (corner && (type & CORNER_TOP_LEFT)) ? radius : 0;
	int radius_tr = (corner && (type & CORNER_TOP_RIGHT)) ? radius : 0;
	int radius_bl = (corner && (type & CORNER_BOTTOM_LEFT)) ? radius : 0;
	int radius_br = (corner && (type & CORNER_BOTTOM_RIGHT)) ? radius : 0;

	//int radius_t = (corner && (type & (CORNER_TOP_LEFT | CORNER_TOP_RIGHT))) ? radius : 0;
	//int radius_b = (corner && (type & (CORNER_BOTTOM_LEFT | CORNER_BOTTOM_RIGHT))) ? radius : 0;

	waitForIdle();

	if (radius_tl) {
		fb_pixel_t *p = accel->lbb + y * stride/sizeof(fb_pixel_t) + x;
		for (int cy = radius - 1; cy > -1; cy--) {
			for (int cx = radius - 1; cx > -1; cx--, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}
	if (radius_tr) {
		fb_pixel_t *p = accel->lbb + y * stride/sizeof(fb_pixel_t) + x + dx - radius_tr;
		for (int cy = radius - 1; cy > -1; cy--) {
			for (int cx = 0; cx < radius; cx++, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}
	if (radius_bl) {
		fb_pixel_t *p = accel->lbb + (y + dy - radius_bl) * stride/sizeof(fb_pixel_t) + x;
		for (int cy = 0; cy < radius; cy++) {
			for (int cx = radius - 1; cx > -1; cx--, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}
	if (radius_br) {
		fb_pixel_t *p = accel->lbb + (y + dy - radius_bl) * stride/sizeof(fb_pixel_t) + x + dx - radius_br;
		for (int cy = 0; cy < radius; cy++) {
			for (int cx = 0; cx < radius; cx++, p++) {
				unsigned char level = corner[cx + cy * radius];
				if (level)
					*p = mergeColor(*p, 255 - level, col, level);
			}
			p += stride/sizeof(fb_pixel_t) - radius;
		}
	}

	for (int i = 0; i < px; i++) {
		paintHLineRel(x + radius_tl, dx - radius_tl - radius_tr, y + i, col); // top
		paintVLineRel(x + i, y + radius_tl, dy - radius_tl - radius_bl, col); // left
		paintVLineRel(x + dx - i - 1, y + radius_tr, dy - radius_tr - radius_br, col); // right
		paintHLineRel(x + radius_tl, dx - radius_bl - radius_br, y + dy - i - 1, col); // bottom
	}
}

void CFrameBuffer::useBackground(bool ub)
{
	useBackgroundPaint = ub;
	if(!useBackgroundPaint) {
		delete[] background;
		background=0;
	}
}

bool CFrameBuffer::getuseBackground(void)
{
	return useBackgroundPaint;
}

void CFrameBuffer::saveBackgroundImage(void)
{
	if (backupBackground != NULL){
		delete[] backupBackground;
		backupBackground = NULL;
	}
	backupBackground = background;
	//useBackground(false); // <- necessary since no background is available
	useBackgroundPaint = false;
	background = NULL;
}

void CFrameBuffer::restoreBackgroundImage(void)
{
	fb_pixel_t * tmp = background;

	if (backupBackground != NULL)
	{
		background = backupBackground;
		backupBackground = NULL;
	}
	else
		useBackground(false); // <- necessary since no background is available

	if (tmp != NULL){
		delete[] tmp;
		tmp = NULL;
	}
}

void CFrameBuffer::paintBackgroundBoxRel(int x, int y, int dx, int dy)
{
	if (!getActive())
		return;

	if(!useBackgroundPaint)
	{
		/* paintBoxRel does its own checkFbArea() */
		paintBoxRel(x, y, dx, dy, backgroundColor);
	}
	else
	{
		checkFbArea(x, y, dx, dy, true);
		uint8_t * fbpos = ((uint8_t *)getFrameBufferPointer()) + x * sizeof(fb_pixel_t) + stride * y;
		fb_pixel_t * bkpos = background + x + BACKGROUNDIMAGEWIDTH * y;
		for(int count = 0;count < dy; count++)
		{
			memmove(fbpos, bkpos, dx * sizeof(fb_pixel_t));
			fbpos += stride;
			bkpos += BACKGROUNDIMAGEWIDTH;
		}
		checkFbArea(x, y, dx, dy, false);
	}
}

void CFrameBuffer::paintBackground()
{
	if (!getActive())
		return;

	if (useBackgroundPaint && (background != NULL))
	{
		checkFbArea(0, 0, xRes, yRes, true);
		/* this does not really work anyway... */
		for (int i = 0; i < 576; i++)
			memmove(((uint8_t *)getFrameBufferPointer()) + i * stride, (background + i * BACKGROUNDIMAGEWIDTH), BACKGROUNDIMAGEWIDTH * sizeof(fb_pixel_t));
		checkFbArea(0, 0, xRes, yRes, false);
	}
	else
	{
		paintBoxRel(0, 0, xRes, yRes, backgroundColor);
	}
	blit();
}

void CFrameBuffer::SaveScreen(int x, int y, int dx, int dy, fb_pixel_t * const memp)
{
	if (!getActive() || memp == NULL)
		return;

	checkFbArea(x, y, dx, dy, true);
	uint8_t * pos = ((uint8_t *)getFrameBufferPointer()) + x * sizeof(fb_pixel_t) + stride * y;
	fb_pixel_t * bkpos = memp;
	for (int count = 0; count < dy; count++) {
		fb_pixel_t * dest = (fb_pixel_t *)pos;
		for (int i = 0; i < dx; i++)
			*(bkpos++) = *(dest++);
		pos += stride;
	}
#if 0
	/* todo: check what the problem with this is, it should be better -- probably caching issue */
	uint8_t * fbpos = ((uint8_t *)getFrameBufferPointer()) + x * sizeof(fb_pixel_t) + stride * y;
	fb_pixel_t * bkpos = memp;
	for (int count = 0; count < dy; count++)
	{
		memmove(bkpos, fbpos, dx * sizeof(fb_pixel_t));
		fbpos += stride;
		bkpos += dx;
	}
#endif
	checkFbArea(x, y, dx, dy, false);
}

void CFrameBuffer::RestoreScreen(int x, int y, int dx, int dy, fb_pixel_t * const memp)
{
	if (!getActive() || memp == NULL)
		return;

	checkFbArea(x, y, dx, dy, true);
	uint8_t * fbpos = ((uint8_t *)getFrameBufferPointer()) + x * sizeof(fb_pixel_t) + stride * y;
	fb_pixel_t * bkpos = memp;
	for (int count = 0; count < dy; count++)
	{
		memmove(fbpos, bkpos, dx * sizeof(fb_pixel_t));
		fbpos += stride;
		bkpos += dx;
	}
	checkFbArea(x, y, dx, dy, false);
}

void CFrameBuffer::Clear()
{
	paintBackground();
	//memset(getFrameBufferPointer(), 0, stride * yRes);
}

bool CFrameBuffer::Lock()
{
	if(locked)
		return false;
	locked = true;
	return true;
}

void CFrameBuffer::Unlock()
{
	locked = false;
}

void * CFrameBuffer::int_convertRGB2FB(unsigned char *rgbbuff, unsigned long x, unsigned long y, int transp, bool alpha)
{
	unsigned long i;
	unsigned int *fbbuff;
	unsigned long count = x * y;

	fbbuff = (unsigned int *) cs_malloc_uncached(count * sizeof(unsigned int));
	if(fbbuff == NULL) {
		printf("convertRGB2FB%s: Error: cs_malloc_uncached\n", ((alpha) ? " (Alpha)" : ""));
		return NULL;
	}

	if (alpha) {
		for(i = 0; i < count ; i++)
			fbbuff[i] = ((rgbbuff[i*4+3] << 24) & 0xFF000000) | 
			            ((rgbbuff[i*4]   << 16) & 0x00FF0000) | 
		        	    ((rgbbuff[i*4+1] <<  8) & 0x0000FF00) | 
			            ((rgbbuff[i*4+2])       & 0x000000FF);
	} else {
		switch (m_transparent) {
			case CFrameBuffer::TM_BLACK:
				for(i = 0; i < count ; i++) {
					transp = 0;
					if(rgbbuff[i*3] || rgbbuff[i*3+1] || rgbbuff[i*3+2])
						transp = 0xFF;
					fbbuff[i] = (transp << 24) | ((rgbbuff[i*3] << 16) & 0xFF0000) | ((rgbbuff[i*3+1] << 8) & 0xFF00) | (rgbbuff[i*3+2] & 0xFF);
				}
				break;
			case CFrameBuffer::TM_INI:
				for(i = 0; i < count ; i++)
					fbbuff[i] = (transp << 24) | ((rgbbuff[i*3] << 16) & 0xFF0000) | ((rgbbuff[i*3+1] << 8) & 0xFF00) | (rgbbuff[i*3+2] & 0xFF);
				break;
			case CFrameBuffer::TM_NONE:
			default:
				for(i = 0; i < count ; i++)
					fbbuff[i] = 0xFF000000 | ((rgbbuff[i*3] << 16) & 0xFF0000) | ((rgbbuff[i*3+1] << 8) & 0xFF00) | (rgbbuff[i*3+2] & 0xFF);
				break;
		}
	}
	return (void *) fbbuff;
}

void * CFrameBuffer::convertRGB2FB(unsigned char *rgbbuff, unsigned long x, unsigned long y, int transp)
{
	return int_convertRGB2FB(rgbbuff, x, y, transp, false);
}

void * CFrameBuffer::convertRGBA2FB(unsigned char *rgbbuff, unsigned long x, unsigned long y)
{
	return int_convertRGB2FB(rgbbuff, x, y, 0, true);
}

void CFrameBuffer::blit2FB(void *fbbuff, uint32_t width, uint32_t height, uint32_t xoff, uint32_t yoff, uint32_t xp, uint32_t yp, bool transp)
{
	accel->blit2FB(fbbuff, width, height, xoff, yoff, xp, yp, transp);
}

void CFrameBuffer::displayRGB(unsigned char *rgbbuff, int x_size, int y_size, int x_pan, int y_pan, int x_offs, int y_offs, bool clearfb, int transp)
{
        void *fbbuff = NULL;

        if(rgbbuff == NULL)
                return;

        /* correct panning */
        if(x_pan > x_size - (int)xRes) x_pan = 0;
        if(y_pan > y_size - (int)yRes) y_pan = 0;

        /* correct offset */
        if(x_offs + x_size > (int)xRes) x_offs = 0;
        if(y_offs + y_size > (int)yRes) y_offs = 0;

        /* blit buffer 2 fb */
        fbbuff = convertRGB2FB(rgbbuff, x_size, y_size, transp);
        if(fbbuff==NULL)
                return;

        /* ClearFB if image is smaller */
        /* if(x_size < (int)xRes || y_size < (int)yRes) */
        if(clearfb)
                CFrameBuffer::getInstance()->Clear();

        blit2FB(fbbuff, x_size, y_size, x_offs, y_offs, x_pan, y_pan);
        cs_free_uncached(fbbuff);
}

void CFrameBuffer::paintMuteIcon(bool paint, int ax, int ay, int dx, int dy, bool paintFrame)
{
	if(paint) {
		if (paintFrame) {
			paintBackgroundBoxRel(ax, ay, dx, dy);
			paintBoxRel(ax, ay, dx, dy, COL_MENUCONTENT_PLUS_0, RADIUS_SMALL);
		}
		int icon_dx=0, icon_dy=0;
		getIconSize(NEUTRINO_ICON_BUTTON_MUTE, &icon_dx, &icon_dy);
		paintIcon(NEUTRINO_ICON_BUTTON_MUTE, ax+((dx-icon_dx)/2), ay+((dy-icon_dy)/2));
	}
	else
		paintBackgroundBoxRel(ax, ay, dx, dy);
	blit();
}
#if HAVE_SPARK_HARDWARE
CFrameBuffer::Mode3D CFrameBuffer::get3DMode()
{
	return mode3D;
}

void CFrameBuffer::set3DMode(Mode3D m)
{
	if (mode3D != m) {
		accel->ClearFB();
		mode3D = m;
		accel->borderColorOld = 0x01010101;
		blit();
	}
}

bool CFrameBuffer::OSDShot(const std::string &name)
{
	struct timeval ts, te;
	gettimeofday(&ts, NULL);

	size_t l = name.find_last_of(".");
	if(l == std::string::npos)
		return false;
	if (name.substr(l) != ".png")
		return false;
	FILE *out = fopen(name.c_str(), "w");
	if (!out)
		return false;

	unsigned int xres = DEFAULT_XRES;
	unsigned int yres = DEFAULT_YRES;
	fb_pixel_t *b = (fb_pixel_t *) accel->lbb;

	if (!g_settings.screenshot_backbuffer) {
		xres = accel->s.xres;
		yres = accel->s.yres;
		b = (fb_pixel_t *) lfb;
	}

	png_bytep row_pointers[yres];
	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
		(png_voidp) NULL, (png_error_ptr) NULL, (png_error_ptr) NULL);
	png_infop info_ptr = png_create_info_struct(png_ptr);

	png_init_io(png_ptr, out);

	for (unsigned int y = 0; y < yres; y++)
		row_pointers[y] = (png_bytep) (b + y * xres);

	png_set_compression_level(png_ptr, g_settings.screenshot_png_compression);
	png_set_bgr(png_ptr);
	png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
	png_set_IHDR(png_ptr, info_ptr, xres, yres, 8, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	png_write_info(png_ptr, info_ptr);
	png_write_image(png_ptr, row_pointers);
	png_write_end(png_ptr, NULL);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	fclose(out);

	gettimeofday(&te, NULL);
	fprintf(stderr, "%s took %lld us\n", __func__, (te.tv_sec * 1000000LL + te.tv_usec) - (ts.tv_sec * 1000000LL + ts.tv_usec));
	return true;
}

void CFrameBuffer::blitArea(int src_width, int src_height, int fb_x, int fb_y, int width, int height)
{
	accel->blitArea(src_width, src_height, fb_x, fb_y, width, height);
}

void CFrameBuffer::resChange(void)
{
	accel->resChange();
}

void CFrameBuffer::setBorder(int sx, int sy, int ex, int ey)
{
	accel->setBorder(sx, sy, ex, ey);
}

void CFrameBuffer::getBorder(int &sx, int &sy, int &ex, int &ey)
{
	accel->getBorder(sx, sy, ex, ey);
}

void CFrameBuffer::setBorderColor(fb_pixel_t col)
{
	accel->setBorderColor(col);
}

fb_pixel_t CFrameBuffer::getBorderColor(void)
{
	return accel->getBorderColor();
}

void CFrameBuffer::ClearFB(void)
{
	accel->ClearFB();
}

void CFrameBuffer::setMixerColor(uint32_t mixer_background)
{
	struct stmfbio_output_configuration outputConfig;
	memset(&outputConfig, 0, sizeof(outputConfig));
	outputConfig.outputid = STMFBIO_OUTPUTID_MAIN;
	outputConfig.activate = STMFBIO_ACTIVATE_IMMEDIATE;
	outputConfig.caps = STMFBIO_OUTPUT_CAPS_MIXER_BACKGROUND;
	outputConfig.mixer_background = mixer_background;
	if(ioctl(fd, STMFBIO_SET_OUTPUT_CONFIG, &outputConfig) < 0)
		perror("setting output configuration failed");
}

void *CFrameBuffer::autoBlitThread(void *arg)
{
	set_threadname("autoblit");
	CFrameBuffer *me = (CFrameBuffer *) arg;
	me->autoBlitThread();
	pthread_exit(NULL);
}

void CFrameBuffer::autoBlitThread(void)
{
	while (autoBlitStatus) {
		blit();
		for (int i = 4; i && autoBlitStatus; i--)
			usleep(50000);
	}
}

void CFrameBuffer::autoBlit(bool b)
{
	if (b && !autoBlitThreadId) {
		autoBlitStatus = true;
		pthread_create(&autoBlitThreadId, NULL, autoBlitThread, this);
	} else if (!b && autoBlitThreadId) {
		autoBlitStatus = false;
		pthread_join(autoBlitThreadId, NULL);
		autoBlitThreadId = 0;
	}
}
#endif

void CFrameBuffer::setFbArea(int element, int _x, int _y, int _dx, int _dy)
{
	if (_x == 0 && _y == 0 && _dx == 0 && _dy == 0) {
		// delete area
		for (fbarea_iterator_t it = v_fbarea.begin(); it != v_fbarea.end(); ++it) {
			if (it->element == element) {
				v_fbarea.erase(it);
				break;
			}
		}
		if (v_fbarea.empty()) {
			fbAreaActiv = false;
		}
	}
	else {
		// change area
		bool found = false;
		for (unsigned int i = 0; i < v_fbarea.size(); i++) {
			if (v_fbarea[i].element == element) {
				v_fbarea[i].x = _x;
				v_fbarea[i].y = _y;
				v_fbarea[i].dx = _dx;
				v_fbarea[i].dy = _dy;
				found = true;
				break;
			}
		}
		// set new area
		if (!found) {
			fb_area_t area;
			area.x = _x;
			area.y = _y;
			area.dx = _dx;
			area.dy = _dy;
			area.element = element;
			v_fbarea.push_back(area);
		}
		fbAreaActiv = true;
	}
}

int CFrameBuffer::checkFbAreaElement(int _x, int _y, int _dx, int _dy, fb_area_t *area)
{
	if (fb_no_check)
		return FB_PAINTAREA_MATCH_NO;

	if (_y > area->y + area->dy)
		return FB_PAINTAREA_MATCH_NO;
	if (_x + _dx < area->x)
		return FB_PAINTAREA_MATCH_NO;
	if (_x > area->x + area->dx)
		return FB_PAINTAREA_MATCH_NO;
	if (_y + _dy < area->y)
		return FB_PAINTAREA_MATCH_NO;
	return FB_PAINTAREA_MATCH_OK;
}

bool CFrameBuffer::_checkFbArea(int _x, int _y, int _dx, int _dy, bool prev)
{
	if (v_fbarea.empty())
		return true;

	for (unsigned int i = 0; i < v_fbarea.size(); i++) {
		int ret = checkFbAreaElement(_x, _y, _dx, _dy, &v_fbarea[i]);
		if (ret == FB_PAINTAREA_MATCH_OK) {
			switch (v_fbarea[i].element) {
				case FB_PAINTAREA_MUTEICON1:
					if (!do_paint_mute_icon)
						break;
					fb_no_check = true;
					if (prev)
						CAudioMute::getInstance()->hide(true);
					else
						CAudioMute::getInstance()->paint();
					fb_no_check = false;
					break;
				default:
					break;
			}
		}
	}

	return true;
}
