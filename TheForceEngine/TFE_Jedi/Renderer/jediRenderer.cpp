#include "jediRenderer.h"
#include <TFE_Jedi/Math/fixedPoint.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Level/level.h>
#include "rcommon.h"
#include "rsectorRender.h"
#include "RClassic_Fixed/rcommonFixed.h"
#include "RClassic_Fixed/rclassicFixed.h"
#include "RClassic_Fixed/rsectorFixed.h"

//#include "RClassic_Float/rclassicFloat.h"
//#include "RClassic_Float/rsectorFloat.h"

#include <TFE_System/profiler.h>
#include <TFE_Asset/spriteAsset_Jedi.h>
#include <TFE_Asset/modelAsset_jedi.h>
#include <TFE_FrontEndUI/console.h>
#include <TFE_System/memoryPool.h>

namespace TFE_Jedi
{
	static s32 s_sectorId = -1;

	static bool s_init = false;
	static MemoryPool s_memPool;
	static TFE_SubRenderer s_subRenderer = TSR_CLASSIC_FIXED;
	ScreenRect s_screenRect = { 0, 1, 319, 198 };

	TFE_Sectors* s_sectorRenderer = nullptr;

	/////////////////////////////////////////////
	// Forward Declarations
	/////////////////////////////////////////////
	void clear1dDepth();
	void console_setSubRenderer(const std::vector<std::string>& args);
	void console_getSubRenderer(const std::vector<std::string>& args);

	/////////////////////////////////////////////
	// Implementation
	/////////////////////////////////////////////
	void renderer_init()
	{
		if (s_init) { return; }
		s_init = true;
		// Setup Debug CVars.
		s_maxWallCount = 0xffff;
		s_maxDepthCount = 0xffff;
		CVAR_INT(s_maxWallCount, "d_maxWallCount", CVFLAG_DO_NOT_SERIALIZE, "Maximum wall count for a given sector.");
		CVAR_INT(s_maxDepthCount, "d_maxDepthCount", CVFLAG_DO_NOT_SERIALIZE, "Maximum adjoin depth count.");

		CCMD("rsetSubRenderer", console_setSubRenderer, 1, "Set the sub-renderer - valid values are: Classic_Fixed, Classic_Float, Classic_GPU");
		CCMD("rgetSubRenderer", console_getSubRenderer, 0, "Get the current sub-renderer.");

		// Setup performance counters.
		TFE_COUNTER(s_maxAdjoinDepth, "Maximum Adjoin Depth");
		TFE_COUNTER(s_maxAdjoinIndex, "Maximum Adjoin Count");
		TFE_COUNTER(s_sectorIndex, "Sector Count");
		TFE_COUNTER(s_flatCount, "Flat Count");
		TFE_COUNTER(s_curWallSeg, "Wall Segment Count");
		TFE_COUNTER(s_adjoinSegCount, "Adjoin Segment Count");

		s_sectorRenderer = new TFE_Sectors_Fixed();
	}

	void renderer_destroy()
	{
	}

	void setupInitCameraAndLights()
	{
		if (s_subRenderer == TSR_CLASSIC_FIXED) { RClassic_Fixed::setupInitCameraAndLights(); }
	}

	void setResolution(s32 width, s32 height)
	{
		if (s_subRenderer == TSR_CLASSIC_FIXED) { RClassic_Fixed::setResolution(width, height); }
		//else { RClassic_Float::setResolution(width, height); }
	}

	void blitTextureToScreen(TextureData* texture, s32 x0, s32 y0)
	{
		if (s_subRenderer == TSR_CLASSIC_FIXED) { RClassic_Fixed::blitTextureToScreen(texture, x0, y0); }
		//else { RClassic_Float::setResolution(width, height); }
	}

	void clear3DView(u8* framebuffer)
	{
		RClassic_Fixed::clear3DView(framebuffer);
	}
	
	void setupLevel(s32 width, s32 height)
	{
		renderer_init();
		setResolution(width, height);

		s_memPool.init(32 * 1024 * 1024, "Classic Renderer - Software");
		s_sectorId = -1;
	}

	void console_setSubRenderer(const std::vector<std::string>& args)
	{
		if (args.size() < 2) { return; }
		const char* value = args[1].c_str();

		s32 width = s_width, height = s_height;
		if (strcasecmp(value, "Classic_Fixed") == 0)
		{
			setSubRenderer(TSR_CLASSIC_FIXED);
			setupLevel(width, height);
		}
		else if (strcasecmp(value, "Classic_Float") == 0)
		{
			setSubRenderer(TSR_CLASSIC_FLOAT);
			setupLevel(width, height);
		}
		else if (strcasecmp(value, "Classic_GPU") == 0)
		{
			setSubRenderer(TSR_CLASSIC_GPU);
			setupLevel(width, height);
		}
	}

	void console_getSubRenderer(const std::vector<std::string>& args)
	{
		const char* c_subRenderers[] =
		{
			"Classic_Fixed",	// TSR_CLASSIC_FIXED
			"Classic_Float",	// TSR_CLASSIC_FLOAT
			"Classic_GPU",		// TSR_CLASSIC_GPU
		};
		TFE_Console::addToHistory(c_subRenderers[s_subRenderer]);
	}

	void renderer_setVisionEffect(s32 effect)
	{
		if (s_subRenderer == TSR_CLASSIC_FIXED) { RClassic_Fixed::setVisionEffect(effect); }
	}

	void setSubRenderer(TFE_SubRenderer subRenderer/* = TSR_CLASSIC_FIXED*/)
	{
		// HACK:
		// Force sub-renderer to fixed for now until the refactoring is complete.
		subRenderer = TSR_CLASSIC_FIXED;

		if (subRenderer != s_subRenderer)
		{
			s_subRenderer = subRenderer;
		}
	}

#if 0
	void setCamera(f32 yaw, f32 pitch, f32 x, f32 y, f32 z, s32 sectorId, s32 worldAmbient, bool cameraLightSource)
	{
		if (s_subRenderer == TSR_CLASSIC_FIXED) { RClassic_Fixed::setCamera(yaw, pitch, x, y, z, sectorId); }
		// else { RClassic_Float::setCamera(yaw, pitch, x, y, z, sectorId); }

		s_sectorId = sectorId;
		s_cameraLightSource = 0;
		s_worldAmbient = worldAmbient;
		s_cameraLightSource = cameraLightSource ? -1 : 0;

		s_drawFrame++;
	}
#endif

	void renderer_setWorldAmbient(s32 value)
	{
		s_worldAmbient = MAX_LIGHT_LEVEL - value;
	}

	void renderer_setupCameraLight(JBool flatShading, JBool headlamp)
	{
		s_enableFlatShading = flatShading;
		s_cameraLightSource = headlamp;
	}

	void drawWorld(u8* display, RSector* sector, const u8* colormap, const u8* lightSourceRamp)
	{
		// Clear the top pixel row.
		memset(display, 0, s_width);

		s_drawFrame++;
		RClassic_Fixed::computeSkyOffsets();

		s_display = display;
		s_colorMap = colormap;
		s_lightSourceRamp = lightSourceRamp;
		clear1dDepth();

		s_windowMinX = s_minScreenX;
		s_windowMaxX = s_maxScreenX;
		s_windowMinY = 1;
		s_windowMaxY = s_height - 1;
		s_windowMaxCeil = s_minScreenY;
		s_windowMinFloor = s_maxScreenY;
		s_flatCount = 0;
		s_nextWall = 0;
		s_curWallSeg = 0;

		s_prevSector = nullptr;
		s_sectorIndex = 0;
		s_maxAdjoinIndex = 0;
		s_adjoinSegCount = 1;
		s_adjoinIndex = 0;

		s_adjoinDepth = 1;
		s_maxAdjoinDepth = 1;

		for (s32 i = 0; i < s_width; i++)
		{
			s_columnTop[i] = s_minScreenY;
			s_columnBot[i] = s_maxScreenY;
			s_windowTop_all[i] = s_minScreenY;
			s_windowBot_all[i] = s_maxScreenY;
		}

		// Recursively draws sectors and their contents (sprites, 3D objects).
		{
			TFE_ZONE("Sector Draw");
			s_sectorRenderer->draw(sector);
		}
	}

	/////////////////////////////////////////////
	// Internal
	/////////////////////////////////////////////
	void clear1dDepth()
	{
		if (s_subRenderer == TSR_CLASSIC_FIXED)
		{
			memset(RClassic_Fixed::s_depth1d_all_Fixed, 0, s_width * sizeof(s32));
			RClassic_Fixed::s_windowMinZ_Fixed = 0;
		}
		else
		{
			memset(s_depth1d_all, 0, s_width * sizeof(f32));
			s_windowMinZ = 0.0f;
		}
	}
}
