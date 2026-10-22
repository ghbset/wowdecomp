-- Video Options > Advanced: the settings our own exes (Wow32.exe / Wow64.exe) add, in custom.MPQ.
-- Loaded by GameUI.cpp after FrameXML.toc and Bindings.xml (not through FrameXML.toc, which stays stock).
-- Sliders, checkboxes and dropdowns apply live and Cancel puts the values back; settings marked
-- restart are only written on Okay/Apply and ask for a client restart, like the stock texture options.

local RESTART = OPTION_RESTART_REQUIREMENT or "Requires a client restart."

-- { cvar, kind, label, tooltip, ... }  kind: "check" | "slider" (min, max, step) | "drop" ({ {value, text}, ... })
local PAGES = {
	{ name = "Advanced", title = "Renderer & Performance",
	  text = "Settings added by this client. Pick a category on the left.",
	  { "gxApi", "drop", "Graphics API", "Which renderer the client uses. D3D11 and Vulkan add multisampled fog, GPU particles and more.",
	    { { "D3D9", "Direct3D 9 (stock)" }, { "D3D11", "Direct3D 11" }, { "Vulkan", "Vulkan" } }, restart = true },
	  { "gxAlphaToCoverage", "check", "Smooth cut-out edges", "Smooths the edges of leaves, grass and fences while multisampling (Resolution page) is on. D3D11 and Vulkan." },
	  { "renderThread", "check", "Render thread", "Hands finished frames to a second thread so the next frame can start sooner. Vulkan only." },
	  { "particleGpuQuads", "check", "GPU particles", "Builds particle sprites on the graphics card instead of the processor. Vulkan only." },
	  { "occlusionCulling", "check", "Occlusion culling", "Skips buildings and objects hidden behind others. Vulkan only; off by default because world models can flicker while moving." },
	  { "lodObjectCull", "check", "Fade tiny far objects", "Small objects only a few pixels tall in the distance fade out instead of being drawn." },
	  { "lodObjectCullSize", "slider", "Tiny object size", "How small an object must look before it fades out (5 = one pixel tall).", 5, 50, 1 },
	  { "horizonStaticVB", "check", "Static horizon buffers", "Draws far building silhouettes from buffers kept on the graphics card." },
	  { "componentThreads", "check", "Threaded character textures", "Builds character skins and armour textures on several processor cores." },
	  { "mpqThreads", "check", "Threaded file loading", "Unpacks game files on several processor cores." },
	  { "worldLoadSort", "check", "Load nearest first", "Loads the trees, buildings and objects closest to you before the ones further away." },
	  { "texUploadLimit", "drop", "Textures per frame", "How many newly loaded textures are set up each frame while you play. Lower spreads the work out and smooths hitches when flying; loading screens are never limited.",
	    { { "8", "8" }, { "16", "16" }, { "24", "24" }, { "48", "48" }, { "0", "Unlimited" } } },
	},
	{ name = "Fog", parent = "Advanced", title = "Fog",
	  { "vfog", "check", "Volumetric fog", "Replaces the stock fog with lit, volumetric fog." },
	  { "vfogQuality", "drop", "Fog quality", "Resolution and step count of the fog. Higher looks smoother and costs more.",
	    { { "1", "Low" }, { "2", "Medium" }, { "3", "High" } } },
	  { "vfogDensity", "slider", "Density", "How thick all fog is.", 0, 10, 0.05 },
	  { "vfogHaze", "slider", "Haze", "How much light haze hangs in the air.", 0, 10, 0.05 },
	  { "vfogGroundFog", "slider", "Ground fog", "How much fog gathers near the ground.", 0, 10, 0.05 },
	  { "vfogFarFog", "slider", "Distance fog", "How much the far distance fades into fog.", 0, 10, 0.05 },
	  { "vfogAmbient", "slider", "Ambient light", "How brightly the sky lights the fog.", 0, 10, 0.05 },
	  { "vfogExposure", "slider", "Brightness", "Brightness of the general fog.", 0, 10, 0.05 },
	  { "vfogClassicExposure", "slider", "Zone fog brightness", "Brightness of the hand-made zone fog.", 0, 10, 0.05 },
	  { "vfogData", "check", "Hand-made zone fog", "Uses fog made for each zone where there is some, instead of fog worked out from the sky." },
	  { "vfogClassicNoise", "check", "Drifting fog banks", "Lets the zone fog drift in banks." },
	  { "vfogNoise", "slider", "Patchiness", "How uneven the fog is.", 0, 1, 0.01 },
	  { "vfogNoiseScale", "slider", "Patch size", "Size of the fog patches; larger values make smaller patches.", 0.001, 1, 0.001 },
	  { "vfogNoiseWind", "slider", "Drift speed", "How fast the fog patches drift, in yards per second.", 0, 10, 0.1 },
	  { "vfogStartOffset", "slider", "Start distance", "How far from the camera the fog begins, in yards.", 0, 100, 1 },
	  { "vfogMaxDistance", "slider", "Maximum distance", "How far the fog is worked out, in yards.", 200, 5000, 100 },
	  { "vfogTemporal", "slider", "Smoothing", "Blends each frame with the last to hide flicker. 0 turns it off.", 0, 0.97, 0.01 },
	  { "vfogInterior", "check", "Thinner fog indoors", "Fades outdoor fog and sunlight inside buildings and caves." },
	  { "vfogInteriorDensity", "slider", "Indoor fog", "How much outdoor fog is kept indoors.", 0, 1, 0.05 },
	  { "vfogInteriorFarFog", "slider", "Indoor distance fog", "How much distance fog is kept indoors. Lower clears dungeon haze.", 0, 1, 0.05 },
	  { "vfogUnderwater", "check", "Fog under water", "Keeps the volumetric fog on while the camera is under water." },
	},
	{ name = "Light & God Rays", parent = "Advanced", title = "Light & God Rays",
	  { "gfxSunArc", "check", "Sun and moon cross the sky", "The sun rises in the east, passes through the south and sets in the west, and the moon does the same at night. The world's light, the shadows and the fog come from wherever the sun or moon is. Off uses the stock sky and light." },
	  { "gfxSunArcMatch", "slider", "Gentle high sun", "How much the high midday sun or moon is kept from lighting the ground harder than the stock sky did. 1 matches the stock brightness, 0 lights the ground from the sun's full height.", 0, 1, 0.05 },
	  { "gfxSunArcDawnRamp", "slider", "Slow sunrise light", "How high (in degrees) the sun or moon must climb before its light reaches full strength, so dawn and dusk light comes in gently. 4 is the old quick ramp.", 4, 30, 1 },
	  { "debugHour", "slider", "Time of day (preview)", "Draws the sky, light, sun, moon, shadows and skyboxes for this hour. -1 follows the game clock. Not saved.", -1, 24, 0.5 },
	  { "vfogGodRays", "slider", "God rays", "Light shafts from the sky around the sun. 0 turns them off.", 0, 4, 0.25 },
	  { "vfogShadowedSun", "check", "Sun shafts through the scene", "Sunlight in the fog is blocked by trees and buildings, so beams fall through the fog where the sun reaches. Needs shadows above Low." },
	  { "vfogCanopy", "slider", "Fog darkening in shade", "How much darker the fog is under trees and beside buildings while sun shafts are on.", 0, 1, 0.05 },
	  { "vfogSunScatter", "slider", "Sun glow in fog", "How much sun or moon light the fog catches.", 0, 10, 0.05 },
	  { "vfogLocalLights", "check", "Lamps light the fog", "Torches, lamps and other nearby lights brighten the fog." },
	  { "vfogLocalLightIntensity", "slider", "Lamp brightness", "How brightly nearby lights shine in the fog.", 0, 8, 0.1 },
	  { "vfogLocalLightPhase", "slider", "Lamp glow spread", "Above 0 the glow gathers around lights in front of you; below 0 behind you.", -0.9, 0.9, 0.05 },
	  { "vfogForeverGlow", "check", "Modern glow", "Uses the modern client's glow strength where its lighting covers the camera." },
	  { "vfogGlowCompensation", "check", "Glow compensation", "Tones the fog down so the full screen glow does not wash it out." },
	},
	{ name = "Colour Grading", parent = "Advanced", title = "Colour Grading",
	  { "vfogGrade", "check", "Colour grading", "Gives the world the modern client's colour look for each zone (not the interface)." },
	  { "vfogGradeStrength", "slider", "Grading strength", "How strong the colour grading is.", 0, 1, 0.05 },
	  { "vfogDither", "drop", "Dithering", "Adds faint noise to hide colour banding in fog, water and grading.",
	    { { "0", "Off" }, { "1", "Light" }, { "2", "Strong" } } },
	  { "vfogHdrTargets", "check", "High precision fog", "Works out fog in higher precision where the graphics card allows it, to avoid banding." },
	  { "vfogColorSpace", "check", "Linear light blending", "Mixes fog light in linear light. Off blends the way the stock client does." },
	},
	{ name = "Water", parent = "Advanced", title = "Water",
	  -- Cata water (waterStyle, WorldClient/CataWater.cpp): the settings below are the Modern style's.
	  { "waterStyle", "drop", "Water style", "Modern: reflections, waves and foam from the volumetric fog (the settings below). Cataclysm: the Cataclysm water surface, with or without volumetric fog.",
	    { { "0", "Modern" }, { "1", "Cataclysm" } } },
	  { "waterCataRipples", "check", "Cataclysm ripples", "Cataclysm water style: rings and wakes where characters move through or jump into water." },
	  { "waterCataReflection", "drop", "Cataclysm reflections", "Cataclysm water style: what the water mirrors. Each step costs more frame time.",
	    { { "0", "Screen only" }, { "1", "Sky" }, { "2", "Sky and terrain" }, { "3", "Sky, terrain and buildings" } } },
	  { "waterCataReflScale", "slider", "Reflection resolution", "Cataclysm water style: the mirror image's size against the screen. Lower is faster and blurrier.", 0.25, 1, 0.05 },
	  { "vfogWater", "check", "Modern water", "Lakes, rivers, sea and pools get reflections, waves and foam. Needs volumetric fog on." },
	  { "vfogWaterQuality", "drop", "Water quality", "Low reflects only the sky; Medium and High also reflect the world.",
	    { { "1", "Low" }, { "2", "Medium" }, { "3", "High" } } },
	  { "vfogWaterWaves", "slider", "Wave height", "How high the waves are. 0 makes the water flat.", 0, 2, 0.05 },
	  { "vfogWaterWind", "slider", "Wind speed", "How fast the wind drives the waves.", 0.5, 10, 0.1 },
	  { "vfogWaterFoam", "slider", "Foam", "How much foam forms on waves and shores.", 0, 2, 0.05 },
	  { "vfogWaterReflections", "slider", "Reflections", "How strong reflections are. 0 reflects only the sky.", 0, 2, 0.05 },
	  { "vfogWaterSpecular", "slider", "Sun highlight", "How bright the sun and moon shine on the water.", 0, 4, 0.1 },
	  { "vfogWaterClarity", "slider", "Clarity", "How clear the water is; higher sees deeper.", 0.25, 4, 0.05 },
	  { "vfogWaterZoneColors", "slider", "Zone colours", "How much each zone's own water colour tints the water.", 0, 1, 0.05 },
	  { "vfogWaterRipples", "slider", "Ripples", "Ripples and wakes where characters move through water. 0 turns them off.", 0, 2, 0.05 },
	  { "vfogWaterClientSplashes", "check", "Stock splashes", "Keeps the stock splash rings and wake trails on top of the ripples." },
	  { "vfogWaterGpuWaves", "check", "Waves on the GPU", "Works out the waves on the graphics card instead of the processor." },
	  -- caustics (docs/BACKLOG_PORT.md "Caustics")
	  { "waterCaustics", "slider", "Caustics", "Rippling sunlight on the ground under shallow water, seen from above and below. Not at night or in shade. 0 turns it off.", 0, 3, 0.05 },
	  { "waterUnderwaterTint", "slider", "Underwater tint", "Tints the screen with the water's colour and darkens the corners while you are under water. 0 turns it off.", 0, 2, 0.05 },
	},
	{ name = "World Effects", parent = "Advanced", title = "World Effects",
	  { "groundDecals", "check", "Ground effects follow terrain", "Spell circles on the ground bend over hills instead of floating flat." },
	  { "wmoNormalRepair", "check", "Repair building lighting", "Fixes dark wedges across flat floors and docks under a low sun. Buildings already loaded keep their old lighting until you relog or travel far away." },
	  { "treeSway", "check", "Trees sway in the wind", "Living trees and bushes lean gently in passing gusts. Their shadows move with them." },
	  { "treeSwayStrength", "slider", "Tree sway amount", "How far trees and bushes lean in the wind.", 0, 3, 0.05 },
	  { "treeLeaves", "check", "Falling leaves", "Leaves drift down from the swaying trees nearest you, more when the wind gusts." },
	  { "treeLeavesDensity", "slider", "Falling leaf amount", "How many leaves each tree can shed at once.", 1, 48, 1 },
	  { "weatherSteadyRate", "check", "Steady weather", "Rain, snow and sand fall just as thickly at any frame rate." },
	  { "selectionCircleStyle", "drop", "Target circle", "The look of the circle under your target.",
	    { { "0", "Stock" }, { "1", "Arrow" }, { "2", "Arrow, no fill" }, { "3", "Ring" }, { "4", "Ring, no fill" } } },
	  { "selectionCircleReach", "drop", "Target circle size", "Size the circle by the model, or by melee reach.",
	    { { "0", "Model size" }, { "1", "Melee reach" } } },
	  { "smoothSplines", "check", "Smooth creature paths", "Creatures round the corners of their paths instead of turning sharply." },
	  { "smoothTurning", "check", "Smooth creature turning", "Creatures turn gradually instead of snapping to a new facing." },
	  { "smoothTurnRate", "slider", "Turn speed", "How fast creatures turn, in degrees per second (twice as fast in combat).", 90, 1080, 30 },
	},
	{ name = "Shadows", parent = "Advanced", title = "Shadows",
	  text = "For the sun shadows set by Shadow Quality on the Graphics page.",
	  { "gfxSunShadows", "check", "Shadows follow the sun", "Shadows fall from where the sun or moon is in the sky, long in the morning and evening and short at noon. Off uses the stock steep light." },
	  { "gfxShadowSnap", "check", "Steady shadow edges", "Moves the shadow maps in whole pixels so shadow edges do not crawl or shimmer as you move." },
	  { "gfxShadowSlopeBias", "slider", "Low sun shadow bias", "Stops striped or speckled shadows on walls and roofs when the sun is low. Higher can lift shadows off the ground slightly; 0 is stock.", 0, 4, 0.25 },
	  { "gfxShadowAnchor", "check", "Steady shadows when jumping", "Keeps the shadow maps in place while you jump, so distant shadows do not shift or redraw." },
	  { "gfxShadowFade", "check", "Shadows fade at dusk and dawn", "Shadows fade out as the sun or moon nears the horizon, so they never jump to the other side when the moon takes over." },
	  { "gfxTerrainShadow", "check", "Mountains block the sun", "Hills and distant mountains block the sunlight in the fog and hide the sun's glare when the sun is behind them." },
	  { "shadowMask", "check", "Mountain shadows on the world", "Hills and distant mountains cast the sun's shadow onto the ground, buildings and creatures. Needs \"Mountains block the sun\"." },
	  { "shadowSoft", "slider", "Mountain shadow softness", "How soft the edge of a mountain's shadow is. 0 is hard.", 0, 4, 0.25 },
	},
	{ name = "Loading & Gameplay", parent = "Advanced", title = "Loading & Gameplay",
	  { "fastLoad", "check", "Fast loading", "Loading screens finish sooner; one processor core stays busy while they are up." },
	  { "terrainPoolMaxMB", "slider", "Terrain memory budget (MB)", "Caps the memory kept for drawing ground at long view distances. Far ground that does not fit is skipped. 0 = no cap.", 0, 1024, 32 },
	  { "spellQueueWindow", "slider", "Spell queue window (ms)", "Lets a spell be pressed this long before the global cooldown ends; it casts the moment the cooldown is over. 0 turns it off.", 0, 400, 10 },
	},
	{ name = "Debug", parent = "Advanced", title = "Debug & Technical",
	  text = "For testing. The defaults are right for normal play.",
	  { "vfogDebug", "drop", "Fog debug view", "Shows one part of the fog on screen.",
	    { { "0", "Off" }, { "1", "Fog light" }, { "2", "Transmittance" }, { "3", "Depth" } } },
	  { "vfogWaterDebug", "drop", "Water debug view", "Shows one part of the water on screen.",
	    { { "0", "Off" }, { "1", "Normals" }, { "2", "Foam" }, { "3", "Transmittance" }, { "4", "Reflection" }, { "5", "Liquid type" }, { "6", "Ripples" } } },
	  { "vfogSunMarker", "check", "Sun marker", "Draws a red dot where the sunlight comes from." },
	  { "vfogStockFog", "check", "Replace stock fog", "Off keeps the stock fog and adds no distance fog." },
	  { "vfogTransparent", "check", "Fog before transparents", "Fogs the world before glass, water and effects and fits the fog onto them." },
	  { "vfogFadeEarly", "check", "Fog fading models early", "Models fading in at their draw distance get fog at their own depth." },
	  { "vfogFadeStencil", "check", "Fading model cross-fade", "Fading models blend between the fog behind them and their own." },
	  { "vfogMsaaSplit", "check", "Multisampled fog edges", "With multisampling, each edge sample gets the fog of its own depth." },
	  { "vfogWaterStencil", "check", "Water stencil mask", "Marks water in the stencil buffer; off uses a depth comparison." },
	  { "vfogLiquidDepth", "check", "Water writes depth", "Water is fogged at its own distance (always on with modern water)." },
	  { "vfogClassicPhase", "check", "Normalised zone sun glow", "Uses energy-normalised sun scattering for the zone fog." },
	},
	{ name = "Post Processing", parent = "Advanced", title = "Anti-Aliasing & Colour",
	  text = "Applied to the 3D world only; names and the interface stay sharp.",
	  { "ffxAntiAliasingMode", "drop", "Post anti-aliasing", "Smooths jagged edges, including leaves and fences multisampling misses. Slightly softens the image.",
	    { { "0", "Off" }, { "1", "FXAA Low" }, { "2", "FXAA High" } } },
	  { "colorblindSimulator", "drop", "Colour blindness type", "The kind of colour blindness to show or correct for.",
	    { { "0", "Off" }, { "1", "Protanopia (red)" }, { "2", "Deuteranopia (green)" }, { "3", "Tritanopia (blue)" } } },
	  { "daltonize", "check", "Correct colours", "On shifts colours so they are easier to tell apart with that colour blindness. Off shows how the world looks with it." },
	  { "colorblindWeaknessFactor", "slider", "Strength", "How strong the colour blindness view or correction is.", 0, 1, 0.05 },
	  { "SSAO", "drop", "Ambient occlusion", "Soft contact shadows in corners, creases and under objects.",
	    { { "0", "Off" }, { "1", "Low (half resolution)" }, { "2", "High (full resolution)" } } },
	  { "SSAOBlur", "drop", "Ambient occlusion blur", "Smooths the occlusion. Edge-aware keeps it off the background behind objects.",
	    { { "0", "Off" }, { "1", "Box" }, { "2", "Edge-aware" } } },
	  { "SSAODistance", "slider", "Ambient occlusion distance", "How far away (yards) the occlusion reaches before it fades out.", 25, 400, 25 },
	  { "SSAOStrength", "slider", "Ambient occlusion strength", "How dark the occlusion is.", 0, 2, 0.1 },
	  { "refraction", "drop", "Heat haze", "Air shimmers above fires, braziers, lava and burning spells.",
	    { { "0", "Off" }, { "1", "On" }, { "2", "High" } } },
	  { "refractionStrength", "slider", "Heat haze strength", "How strongly the hot air bends what is behind it.", 0, 4, 0.25 },
	},
}

-- Grass wind (wxl-grasswind, DetailDoodad.hlsl): its own page, placed before Debug.
table.insert(PAGES, #PAGES, { name = "Grass", parent = "Advanced", title = "Grass",
	{ "grassWind", "check", "Grass sways in the wind", "Grass moves gently in the wind." },
	{ "grassWindStrength", "slider", "Wind strength", "How far the grass sways.", 0, 4, 0.1 },
	{ "grassParting", "check", "Grass parts as you walk", "Grass leans away from your character and settles back behind you." },
})

-- Lava and slime (backlog #10, docs/BACKLOG_PORT.md): on the Water page.
for _, page in ipairs(PAGES) do
	if page.name == "Water" then
		table.insert(page, { "liquidProcMagma", "check", "Cataclysm lava and slime", "Lava and slime flow with layered, moving textures as in Cataclysm. Off keeps the stock look." })
	end
end

-- Lamp light (Legion lightMode, docs/LEGION_PORT.md "Legion lights"): its own page, placed before the last one.
table.insert(PAGES, #PAGES, { name = "Lights", parent = "Advanced", title = "Lamps & Torches",
	{ "lightMode", "drop", "Lamp light on the world", "Lamps, torches and braziers light the ground and walls around them.",
	  { { "0", "Off" }, { "1", "Ground and buildings" }, { "2", "Also creatures and grass" } } },
	{ "maxLightCount", "slider", "Most lamps at once", "How many of the nearest lamps light the world at the same time.", 1, 128, 1 },
	{ "maxLightDist", "slider", "Lamp light distance (yards)", "How far away lamps still light the world.", 20, 2048, 20 },
	{ "lightIntensity", "slider", "Lamp brightness", "How bright the lamp light on the world is.", 0, 4, 0.1 },
})
-- Soft particles (backlog #4, docs/BACKLOG_PORT.md): on the World Effects page.
for _, page in ipairs(PAGES) do
	if page.name == "World Effects" then
		table.insert(page, { "softParticles", "check", "Soft particles", "Smoke, fire and spell effects fade where they meet the ground, walls and water instead of cutting a hard line." })
		table.insert(page, { "softParticlesDistance", "slider", "Soft particle fade (yards)", "How far in front of the ground an effect starts to fade.", 0.1, 5, 0.1 })
	end
end

-- Badgermilk0 ports (perfcore, streaming, gpuskin, shadows), lighting post passes and the sky: placed before Debug.
table.insert(PAGES, #PAGES, { name = "Performance", parent = "Advanced", title = "Performance & Streaming",
	{ "gxMaxFrameLatency", "drop", "Frames queued ahead", "How many frames the graphics card may queue while the next one is prepared. Fewer feels more responsive, more runs smoother. All renderers.",
	  { { "0", "0 (wait for each frame)" }, { "1", "1" }, { "2", "2" }, { "3", "3" } } },
	{ "textureCacheSize", "slider", "Texture cache (MB)", "Memory kept for textures no longer on screen, so they come back without reloading. All renderers.", 32, 1024, 32 },
	{ "SoundMemoryCache", "slider", "Sound cache (MB)", "Memory kept for sounds already played, so they are not read from disk again. All renderers.", 4, 256, 4, restart = true },
	{ "groundEffectBuffers", "slider", "Grass buffers", "Grass batches kept ready per pool. More avoids rebuilding grass when you turn or move fast. All renderers.", 32, 1024, 32 },
	{ "groundEffectJobs", "check", "Threaded grass", "Builds grass on several processor cores. All renderers." },
	{ "asyncReadThreads", "slider", "Disk reader threads", "How many threads read game files from disk. 0 picks a count from your processor. All renderers.", 0, 8, 1, restart = true },
	{ "asyncPostloadBudget", "slider", "Loading time per frame (ms)", "How long each frame may spend setting up files that finished loading. Higher loads faster, lower hitches less. All renderers.", 0, 50, 1 },
	{ "textureLoadBudget", "slider", "Texture read-ahead (MB)", "How much texture data is read ahead of being needed. All renderers.", 4, 512, 4 },
	{ "M2CacheTime", "slider", "Keep unused models (seconds)", "How long a model no longer in view is kept for reuse. All renderers.", 10, 600, 10 },
	{ "M2CacheSize", "slider", "Unused model memory (MB)", "Memory kept for models no longer in view. 0 = no cap. All renderers.", 0, 1024, 32 },
	{ "wmoCacheTime", "slider", "Keep unused buildings (seconds)", "How long a building no longer in view is kept for reuse. All renderers.", 10, 600, 10 },
	{ "portraitLive", "check", "Animated portraits", "Unit portraits are moving 3D models instead of still pictures. All renderers." },
	{ "portraitResolution", "drop", "Portrait resolution", "Size the unit portraits are drawn at. All renderers.",
	  { { "64", "64 (stock)" }, { "128", "128" }, { "256", "256" }, { "512", "512" } } },
	{ "minimapIndoorResolution", "drop", "Indoor minimap resolution", "Size the indoor minimap is drawn at. All renderers.",
	  { { "256", "256 (stock)" }, { "512", "512" }, { "1024", "1024" }, { "2048", "2048" } } },
})
-- Shadow map options (Badgermilk0 port, ShadowQuery.cpp): on the Shadows page.
for _, page in ipairs(PAGES) do
	if page.name == "Shadows" then
		table.insert(page, { "shadowMapSize", "drop", "Shadow map size", "Sharpness of the sun shadows. Larger costs graphics memory. All renderers.",
		  { { "0", "Stock" }, { "1024", "1024" }, { "2048", "2048" }, { "4096", "4096" }, { "8192", "8192" } } })
		table.insert(page, { "shadowSoftness", "slider", "Shadow softness", "How soft shadow edges are. 1 is stock. All renderers.", 0.25, 4, 0.25 })
		table.insert(page, { "shadowCascadeReuse", "check", "Reuse far shadows", "At the highest shadow quality, far shadows are redrawn every few frames instead of every frame. All renderers." })
	end
end
-- World lighting post passes (WorldLighting.cpp).
table.insert(PAGES, #PAGES, { name = "Lighting", parent = "Advanced", title = "HDR, Bloom & Occlusion",
	{ "hdr", "check", "HDR", "Draws the world in high dynamic range and tone maps it. D3D11/Vulkan only." },
	{ "hdrExposure", "slider", "Exposure", "Overall brightness of the world with HDR on. D3D11/Vulkan only.", 0.25, 4, 0.05 },
	{ "hdrTonemap", "drop", "Tone mapping", "How bright light is fitted to the screen with HDR on. D3D11/Vulkan only.",
	  { { "0", "Clamp (stock)" }, { "1", "Neutral" }, { "2", "ACES (filmic)" } } },
	{ "bloom", "slider", "Bloom", "Bright light glows onto what is around it. 0 turns it off. D3D11/Vulkan only.", 0, 2, 0.05 },
	{ "bloomThreshold", "slider", "Bloom threshold", "How bright something must be before it glows. D3D11/Vulkan only.", 0.05, 2, 0.05 },
	{ "lightingSsao", "slider", "Post-pass ambient occlusion", "A second, lighter ambient occlusion drawn after the world, separate from the one on Post Processing. 0 turns it off. D3D11/Vulkan only.", 0, 2, 0.05 },
	{ "lightingSsaoRadius", "slider", "Post-pass occlusion radius (yards)", "How far around each point the post-pass occlusion looks for nearby surfaces. D3D11/Vulkan only.", 0.25, 5, 0.25 },
	{ "lightingSsaoDistance", "slider", "Post-pass occlusion distance (yards)", "How far away the post-pass occlusion reaches before it fades out. D3D11/Vulkan only.", 25, 500, 25 },
	{ "fogScattering", "slider", "Sun glow in distance fog", "Tints the distance fog toward the sun's colour around the sun. 0 turns it off. D3D11/Vulkan only.", 0, 2, 0.05 },
	{ "lightPerPixel", "check", "Per-pixel lighting", "Models, buildings and terrain are lit per pixel instead of per vertex. D3D11/Vulkan only." },
	{ "lightDynamic", "check", "Dynamic lights", "Torches, braziers, lanterns and the lamps inside buildings light the surfaces around them. D3D11/Vulkan only." },
	{ "lightDynamicIntensity", "slider", "Dynamic light intensity", "Overall strength of the dynamic lights. D3D11/Vulkan only.", 0, 8, 0.1 },
	{ "lightDynamicM2", "slider", "Torch and brazier lights", "Strength of the lights carried by models. D3D11/Vulkan only.", 0, 8, 0.1 },
	{ "lightDynamicWmo", "slider", "Building lights", "Strength of the lamps placed in buildings. D3D11/Vulkan only.", 0, 8, 0.1 },
	{ "lightDynamicDistance", "slider", "Dynamic light distance (yards)", "How far from the camera dynamic lights are looked for. D3D11/Vulkan only.", 20, 500, 10 },
	{ "lightFlicker", "slider", "Flame flicker", "How much warm flame lights waver. 0 keeps them steady. D3D11/Vulkan only.", 0, 1, 0.05 },
	{ "lightWrap", "slider", "Light wrap", "How far dynamic light reaches round the edge of a surface. D3D11/Vulkan only.", 0, 1, 0.05 },
	{ "lightSpecular", "check", "Specular highlights", "Shiny buildings and models catch highlights from the sun and dynamic lights. D3D11/Vulkan only." },
	{ "lightSpecularM2", "slider", "Model highlights", "Strength of the highlights on shiny models. D3D11/Vulkan only.", 0, 4, 0.05 },
	{ "lightSpecularWmo", "slider", "Building highlights", "Strength of the highlights on shiny buildings. D3D11/Vulkan only.", 0, 4, 0.05 },
	{ "lightSkyAmbient", "slider", "Sky ambient", "Shadowed sides take the sky's colour from above and the ground's from below. 0 turns it off. D3D11/Vulkan only.", 0, 1, 0.05 },
})
-- Sky and clouds (SkyClouds.cpp, DayNight.cpp).
table.insert(PAGES, #PAGES, { name = "Sky", parent = "Advanced", title = "Sky & Clouds",
	{ "skyClouds", "check", "Volumetric clouds", "Clouds with depth replace the flat stock cloud layer; with the sun crossing the sky it also uses the modern sky colours. All renderers." },
	{ "skyCloudCover", "slider", "Cloud cover", "How cloudy fair weather is where the zone has no clouds of its own. All renderers.", 0, 1, 0.05 },
	{ "skyCloudSpeed", "slider", "Cloud drift speed", "How fast the clouds drift and change shape. All renderers.", 0, 4, 0.1 },
	{ "skyCloudsReflect", "check", "Clouds in water", "Water reflects the volumetric clouds instead of the stock cloud layer. All renderers." },
	{ "gfxSunArcSkybox", "drop", "Zone sky models", "While the sun crosses the sky, which of the zone's sky models are kept. All renderers.",
	  { { "0", "None" }, { "1", "All (stock)" }, { "2", "Only auroras and overlays" } } },
})

local function Num(v)
	return tonumber(v) or 0
end

local function Fmt(v)
	local s = string.format("%.3f", v):gsub("0+$", ""):gsub("%.$", "")
	if s == "-0" then s = "0" end
	return s
end

local function Same(a, b)
	if a == nil or b == nil then return a == b end
	if tonumber(a) and tonumber(b) then return math.abs(tonumber(a) - tonumber(b)) < 0.0005 end
	return string.lower(a) == string.lower(b)
end

local function Tooltip(self)
	local c = self.control or self
	GameTooltip:SetOwner(c, "ANCHOR_RIGHT")
	GameTooltip:SetText(c.label, 1, 1, 1)
	GameTooltip:AddLine(c.tip, nil, nil, nil, 1)
	if c.unavailable then
		GameTooltip:AddLine("Not available with the current renderer.", 1, 0.2, 0.2, 1)
	elseif c.restart then
		GameTooltip:AddLine(RESTART, 1, 0.2, 0.2, 1)
	end
	GameTooltip:Show()
end

local function HideTooltip()
	GameTooltip:Hide()
end

local function DropText(c, value)
	for _, o in ipairs(c.options) do
		if Same(o[1], value) then return o[2] end
	end
	return value or ""
end

-- Shows a value on the control without applying it.
local function Show(c, value)
	c.cur = value
	if c.kind == "check" then
		c:SetChecked(value ~= nil and Num(value) ~= 0)
	elseif c.kind == "slider" then
		c.panel.refreshing = true
		c:SetValue(Num(value))
		c.panel.refreshing = nil
		_G[c:GetName() .. "Text"]:SetText(c.label .. ": " .. (value and Fmt(Num(value)) or "-"))
	else
		UIDropDownMenu_SetText(c, DropText(c, value))
	end
end

-- A change from the user: live controls apply now, restart controls wait for Okay.
local function Change(c, value)
	if c.unavailable or Same(c.cur, value) then return end
	Show(c, value)
	if not c.restart then
		SetCVar(c.cvar, value)
	end
	if VideoOptionsFrameApply then VideoOptionsFrameApply:Enable() end
end

local function Okay(panel)
	for _, c in ipairs(panel.controls) do
		if not c.unavailable and not Same(c.cur, c.orig) then
			SetCVar(c.cvar, c.cur)
			if c.restart then VideoOptionsFrame.gameRestart = true end
			c.orig = c.cur
		end
	end
end

local function Cancel(panel)
	for _, c in ipairs(panel.controls) do
		if not c.unavailable and not Same(c.cur, c.orig) then
			if not c.restart then SetCVar(c.cvar, c.orig) end
			Show(c, c.orig)
		end
	end
end

local function Default(panel)
	for _, c in ipairs(panel.controls) do
		if GetCVar(c.cvar) ~= nil then
			local d = GetCVarDefault(c.cvar)
			if not Same(GetCVar(c.cvar), d) then
				SetCVar(c.cvar, d)
				if c.restart then VideoOptionsFrame.gameRestart = true end
			end
		end
	end
end

local function Refresh(panel)
	for _, c in ipairs(panel.controls) do
		local v = GetCVar(c.cvar)   -- nil until the backend that owns it registers it
		c.unavailable = v == nil
		c:SetAlpha(c.unavailable and 0.4 or 1)
		c.orig = v
		Show(c, v)
	end
end

local function DropInit(self)
	local c = self.control or self
	for _, o in ipairs(c.options) do
		local info = UIDropDownMenu_CreateInfo()
		info.text = o[2]
		info.value = o[1]
		info.checked = Same(c.cur, o[1])
		info.func = function(b) Change(c, b.value) end
		UIDropDownMenu_AddButton(info)
	end
end

local count = 0
local function Build(page)
	count = count + 1
	local name = "VideoOptionsAdvancedPanel" .. count
	local panel = CreateFrame("Frame", name, VideoOptionsFramePanelContainer)
	panel.name = page.name
	panel.parent = page.parent
	panel.okay, panel.cancel, panel.default, panel.refresh = Okay, Cancel, Default, Refresh
	panel.controls = {}
	panel:Hide()

	local title = panel:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
	title:SetPoint("TOPLEFT", 16, -16)
	title:SetText(page.title)
	local sub = panel:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
	sub:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -6)
	sub:SetText(page.text or "")

	local scroll = CreateFrame("ScrollFrame", name .. "Scroll", panel, "UIPanelScrollFrameTemplate")
	scroll:SetPoint("TOPLEFT", 8, -52)
	scroll:SetPoint("BOTTOMRIGHT", -30, 8)
	local content = CreateFrame("Frame", name .. "Content", scroll)
	content:SetWidth(360)
	scroll:SetScrollChild(content)

	local y = -6
	for i, s in ipairs(page) do
		local cname = name .. "Control" .. i
		local c
		if s[2] == "check" then
			c = CreateFrame("CheckButton", cname, content, "OptionsCheckButtonTemplate")
			c:SetPoint("TOPLEFT", 8, y)
			_G[cname .. "Text"]:SetText(s[3])
			c:SetScript("OnClick", function(self)
				PlaySound(self:GetChecked() and "igMainMenuOptionCheckBoxOn" or "igMainMenuOptionCheckBoxOff")
				Change(self, self:GetChecked() and "1" or "0")
				if self.unavailable then Show(self, self.cur) end
			end)
			y = y - 28
		elseif s[2] == "slider" then
			c = CreateFrame("Slider", cname, content, "OptionsSliderTemplate")
			c:SetPoint("TOPLEFT", 16, y - 16)
			c:SetWidth(300)
			c:SetMinMaxValues(s[5], s[6])
			c:SetValueStep(s[7])
			c.step = s[7]
			_G[cname .. "Low"]:SetText(Fmt(s[5]))
			_G[cname .. "High"]:SetText(Fmt(s[6]))
			c:SetScript("OnValueChanged", function(self, value)
				if self.panel.refreshing then return end
				value = math.floor((value - s[5]) / self.step + 0.5) * self.step + s[5]
				Change(self, Fmt(value))
				if self.unavailable then Show(self, self.cur) end
			end)
			y = y - 50
		else
			local label = content:CreateFontString(nil, "ARTWORK", "GameFontNormal")
			label:SetPoint("TOPLEFT", 16, y - 4)
			label:SetText(s[3])
			c = CreateFrame("Frame", cname, content, "UIDropDownMenuTemplate")
			c:SetPoint("TOPLEFT", 150, y)
			c.options = s[5]
			UIDropDownMenu_SetWidth(c, 160)
			UIDropDownMenu_Initialize(c, DropInit)
			c:EnableMouse(true)
			local button = _G[cname .. "Button"]
			button.control = c
			button:HookScript("OnEnter", Tooltip)
			button:HookScript("OnLeave", HideTooltip)
			y = y - 34
		end
		c.cvar, c.kind, c.label, c.tip, c.restart, c.panel = s[1], s[2], s[3], s[4], s.restart, panel
		c:SetScript("OnEnter", Tooltip)
		c:SetScript("OnLeave", HideTooltip)
		tinsert(panel.controls, c)
	end
	content:SetHeight(-y + 8)

	OptionsFrame_AddCategory(VideoOptionsFrame, panel)
end

for _, page in ipairs(PAGES) do
	Build(page)
end
