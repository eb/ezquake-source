/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// glm_surf.c: surface-related refresh code (modern OpenGL)

#include "quakedef.h"
#include "gl_model.h"
#include "gl_local.h"
#include "rulesets.h"
#include "utils.h"
#include "tr_types.h"

#define MAX_WORLDMODEL_BATCH 32

static GLuint modelIndexes[16 * 1024];
static GLuint index_count;
static qbool uniforms_set = false;
extern texture_ref lightmap_texture_array;

static glm_program_t drawworld;
static GLint drawworld_RefdefCvars_block;
static GLint drawworld_WorldCvars_block;

typedef struct block_world_s {
	GLint samplerMappings[MAX_WORLDMODEL_BATCH][4];

	//
	float waterAlpha;

	// drawflat for solid surfaces
	int r_drawflat;
	int r_fastturb;
	int r_fastsky;

	// ! No padding required as above is 16 bytes...

	float r_wallcolor[4];  // only used if r_drawflat 1 or 3
	float r_floorcolor[4]; // only used if r_drawflat 1 or 2

	// drawflat for turb surfaces
	float r_telecolor[4];
	float r_lavacolor[4];
	float r_slimecolor[4];
	float r_watercolor[4];

	// drawflat for sky
	float r_skycolor[4];
	int r_texture_luma_fb;
} block_world_t;

typedef struct glm_brushmodel_req_s {
	// This is DrawElementsIndirectCmd, from OpenGL spec
	GLuint count;           // Number of indexes to pull
	GLuint instanceCount;   // Always 1... ?
	GLuint firstIndex;      // Position of first index in array
	GLuint baseVertex;      // Offset of vertices in VBO
	GLuint baseInstance;    // We use this to pull from array of uniforms in shader

	texture_ref texture_array;
} glm_worldmodel_req_t;

static glm_worldmodel_req_t worldmodel_requests[MAX_WORLDMODEL_BATCH];
static GLuint batch_count = 0;
static glm_vbo_t vbo_worldIndirectDraw;

#define DRAW_DETAIL_TEXTURES 1
#define DRAW_CAUSTIC_TEXTURES 2
#define DRAW_LUMA_TEXTURES 4
static int drawworld_compiledOptions;
static glm_ubo_t ubo_worldcvars;
static block_world_t world;

#define MAXIMUM_MATERIAL_SAMPLERS 32
static int material_samplers_max;
static int material_samplers;
static texture_ref allocated_samplers[MAXIMUM_MATERIAL_SAMPLERS];
static int TEXTURE_UNIT_MATERIAL;
static int TEXTURE_UNIT_LIGHTMAPS;
static int TEXTURE_UNIT_DETAIL;
static int TEXTURE_UNIT_CAUSTICS;

// We re-compile whenever certain options change, to save texture bindings/lookups
static void Compile_DrawWorldProgram(qbool detail_textures, qbool caustic_textures, qbool luma_textures)
{
	int drawworld_desiredOptions =
		(detail_textures ? DRAW_DETAIL_TEXTURES : 0) |
		(caustic_textures ? DRAW_CAUSTIC_TEXTURES : 0) |
		(luma_textures ? DRAW_LUMA_TEXTURES : 0);

	if (!drawworld.program || drawworld_compiledOptions != drawworld_desiredOptions) {
		static char included_definitions[512];
		int samplers = 0;
		
		memset(included_definitions, 0, sizeof(included_definitions));
		if (detail_textures) {
			TEXTURE_UNIT_DETAIL = samplers++;

			strlcat(included_definitions, "#define DRAW_DETAIL_TEXTURES\n", sizeof(included_definitions));
			strlcat(included_definitions, va("#define SAMPLER_DETAIL_TEXTURE %d\n", TEXTURE_UNIT_MATERIAL), sizeof(included_definitions));
			++samplers;
		}
		if (caustic_textures) {
			TEXTURE_UNIT_CAUSTICS = samplers++;

			strlcat(included_definitions, "#define DRAW_CAUSTIC_TEXTURES\n", sizeof(included_definitions));
			strlcat(included_definitions, va("#define SAMPLER_CAUSTIC_TEXTURE %d\n", TEXTURE_UNIT_CAUSTICS), sizeof(included_definitions));
		}
		if (luma_textures) {
			strlcat(included_definitions, "#define DRAW_LUMA_TEXTURES\n", sizeof(included_definitions));
		}
		TEXTURE_UNIT_LIGHTMAPS = samplers++;
		strlcat(included_definitions, va("#define SAMPLER_LIGHTMAP_TEXTURE %d\n", TEXTURE_UNIT_LIGHTMAPS), sizeof(included_definitions));
		TEXTURE_UNIT_MATERIAL = samplers++;
		strlcat(included_definitions, va("#define SAMPLER_MATERIAL_TEXTURE_START %d\n", TEXTURE_UNIT_MATERIAL), sizeof(included_definitions));
		material_samplers_max = min(glConfig.texture_units - TEXTURE_UNIT_MATERIAL, MAXIMUM_MATERIAL_SAMPLERS);
		strlcat(included_definitions, va("#define SAMPLER_MATERIAL_TEXTURE_COUNT %d\n", material_samplers_max), sizeof(included_definitions));
		strlcat(included_definitions, va("#define MAX_INSTANCEID %d\n", MAX_WORLDMODEL_BATCH), sizeof(included_definitions));

		GL_VFDeclare(drawworld);

		// Initialise program for drawing image
		GLM_CreateVFProgramWithInclude("DrawWorld", GL_VFParams(drawworld), &drawworld, included_definitions);

		drawworld_compiledOptions = drawworld_desiredOptions;
	}

	if (drawworld.program && !drawworld.uniforms_found) {
		GLint size;

		drawworld_RefdefCvars_block = glGetUniformBlockIndex(drawworld.program, "RefdefCvars");
		glUniformBlockBinding(drawworld.program, drawworld_RefdefCvars_block, GL_BINDINGPOINT_REFDEF_CVARS);

		drawworld_WorldCvars_block = glGetUniformBlockIndex(drawworld.program, "WorldCvars");
		glGetActiveUniformBlockiv(drawworld.program, drawworld_WorldCvars_block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);

		glUniformBlockBinding(drawworld.program, drawworld_WorldCvars_block, GL_BINDINGPOINT_DRAWWORLD_CVARS);
		GL_GenUniformBuffer(&ubo_worldcvars, "world-cvars", &world, sizeof(world));
		GL_BindUniformBufferBase(&ubo_worldcvars, GL_BINDINGPOINT_DRAWWORLD_CVARS, ubo_worldcvars.ubo);

		drawworld.uniforms_found = true;
	}

	if (!vbo_worldIndirectDraw.vbo) {
		GL_GenFixedBuffer(&vbo_worldIndirectDraw, GL_DRAW_INDIRECT_BUFFER, "world-indirect", sizeof(worldmodel_requests), NULL, GL_STREAM_DRAW);
	}
}

#define PASS_COLOR_AS_4F(target, cvar) \
{ \
	target[0] = (cvar.color[0] * 1.0f / 255); \
	target[1] = (cvar.color[1] * 1.0f / 255); \
	target[2] = (cvar.color[2] * 1.0f / 255); \
	target[3] = 1.0f; \
}

static void GLM_EnterBatchedWorldRegion(qbool detail_tex, qbool caustics, qbool lumas)
{
	extern glm_vao_t brushModel_vao;
	extern glm_vbo_t vbo_brushElements;
	extern cvar_t gl_lumaTextures;

	float wateralpha = bound((1 - r_refdef2.max_watervis), r_wateralpha.value, 1);
	extern cvar_t r_telecolor, r_lavacolor, r_slimecolor, r_watercolor, r_fastturb, r_skycolor;

	Compile_DrawWorldProgram(detail_tex, caustics, lumas);

	world.waterAlpha = wateralpha;

	world.r_drawflat = r_drawflat.integer;
	PASS_COLOR_AS_4F(world.r_wallcolor, r_wallcolor);
	PASS_COLOR_AS_4F(world.r_floorcolor, r_floorcolor);

	world.r_fastturb = r_fastturb.integer;
	PASS_COLOR_AS_4F(world.r_telecolor, r_telecolor);
	PASS_COLOR_AS_4F(world.r_lavacolor, r_lavacolor);
	PASS_COLOR_AS_4F(world.r_slimecolor, r_slimecolor);
	PASS_COLOR_AS_4F(world.r_watercolor, r_watercolor);

	world.r_fastsky = r_fastsky.integer;
	PASS_COLOR_AS_4F(world.r_skycolor, r_skycolor);

	world.r_texture_luma_fb = gl_fb_bmodels.integer ? 1 : 0;

	GL_UseProgram(drawworld.program);

	GL_BindVertexArray(&brushModel_vao);

	// Bind lightmap array
	GL_BindTextureUnit(GL_TEXTURE0 + TEXTURE_UNIT_LIGHTMAPS, GL_TEXTURE_2D_ARRAY, lightmap_texture_array);
	if (detail_tex) {
		GL_BindTextureUnit(GL_TEXTURE0 + TEXTURE_UNIT_DETAIL, GL_TEXTURE_2D, detailtexture);
	}
	if (caustics) {
		GL_BindTextureUnit(GL_TEXTURE0 + TEXTURE_UNIT_CAUSTICS, GL_TEXTURE_2D, underwatertexture);
	}

	Con_DPrintf("Water-alpha offset: %d\n", (intptr_t)&world.waterAlpha - (intptr_t)&world);
	material_samplers = 0;
}

void GLM_ExitBatchedPolyRegion(void)
{
	uniforms_set = false;

	Cvar_SetValue(&developer, 0);
}

static void GL_FlushWorldModelBatch(void);

static glm_worldmodel_req_t* GLM_NextBatchRequest(model_t* model, float* base_color, texture_ref texture_array)
{
	glm_worldmodel_req_t* req;

	if (batch_count >= MAX_WORLDMODEL_BATCH) {
		GL_FlushWorldModelBatch();
	}

	req = &worldmodel_requests[batch_count];

	req->count = 0;
	req->texture_array = texture_array;
	req->instanceCount = 1;
	req->firstIndex = index_count;
	req->baseVertex = 0;
	req->baseInstance = batch_count;
	if (!GL_TextureReferenceIsValid(texture_array)) {
		world.samplerMappings[batch_count][0] = 0;
	}
	else if (material_samplers && GL_TextureReferenceEqual(texture_array, allocated_samplers[material_samplers - 1])) {
		Con_DPrintf("  %2d: sampler %d (array %d)\n", batch_count, material_samplers - 1, texture_array);
		world.samplerMappings[batch_count][0] = material_samplers - 1;
	}
	else {
		GL_BindTextureUnit(GL_TEXTURE0 + TEXTURE_UNIT_MATERIAL + material_samplers, GL_TEXTURE_2D_ARRAY, texture_array);
		world.samplerMappings[batch_count][0] = material_samplers;
		Con_DPrintf("  %2d: sampler %d (array %d) - new\n", batch_count, material_samplers, texture_array);
		allocated_samplers[material_samplers++] = texture_array;
	}

	++batch_count;
	return req;
}

void GLM_DrawTexturedWorld(model_t* model)
{
	extern cvar_t gl_lumaTextures;

	int i, waterline, v;
	msurface_t* surf;
	qbool draw_detail_texture = gl_detail.integer && GL_TextureReferenceIsValid(detailtexture);
	qbool draw_caustics = gl_caustics.integer && GL_TextureReferenceIsValid(underwatertexture);
	qbool draw_lumas = gl_lumaTextures.integer && r_refdef2.allow_lumas;
	glm_worldmodel_req_t* req = NULL;

	GLM_EnterBatchedWorldRegion(draw_detail_texture, draw_caustics, draw_lumas);

	for (waterline = 0; waterline < 2; waterline++) {
		for (surf = model->drawflat_chain[waterline]; surf; surf = surf->drawflatchain) {
			glpoly_t* poly;

			req = GLM_NextBatchRequest(model, NULL, null_texture_reference);
			for (poly = surf->polys; poly; poly = poly->next) {
				int newVerts = poly->numverts;

				if (index_count + 1 + newVerts > sizeof(modelIndexes) / sizeof(modelIndexes[0])) {
					GL_FlushWorldModelBatch();
					req = GLM_NextBatchRequest(model, NULL, null_texture_reference);
				}

				if (index_count) {
					modelIndexes[index_count++] = ~(GLuint)0;
					req->count++;
				}

				for (v = 0; v < newVerts; ++v) {
					modelIndexes[index_count++] = poly->vbo_start + v;
					req->count++;
				}
			}
		}
	}

	for (i = 0; i < model->texture_array_count; ++i) {
		texture_t* base_tex = model->textures[model->texture_array_first[i]];
		qbool first_in_this_array = true;
		int texIndex;

		if (!base_tex || !base_tex->size_start || !GL_TextureReferenceIsValid(base_tex->gl_texture_array)) {
			continue;
		}

		for (texIndex = model->texture_array_first[i]; texIndex >= 0 && texIndex < model->numtextures; texIndex = model->textures[texIndex]->next_same_size) {
			texture_t* tex = model->textures[texIndex];

			if (!tex->texturechain[0] && !tex->texturechain[1]) {
				continue;
			}

			req = GLM_NextBatchRequest(model, NULL, tex->gl_texture_array);
			for (waterline = 0; waterline < 2; waterline++) {
				for (surf = tex->texturechain[waterline]; surf; surf = surf->texturechain) {
					glpoly_t* poly;

					for (poly = surf->polys; poly; poly = poly->next) {
						int newVerts = poly->numverts;

						if (index_count + 1 + newVerts > sizeof(modelIndexes) / sizeof(modelIndexes[0])) {
							GL_FlushWorldModelBatch();
							req = GLM_NextBatchRequest(model, NULL, tex->gl_texture_array);
						}

						if (req->count) {
							modelIndexes[index_count++] = ~(GLuint)0;
							req->count++;
						}

						for (v = 0; v < newVerts; ++v) {
							modelIndexes[index_count++] = poly->vbo_start + v;
							req->count++;
						}
					}
				}
			}
		}
	}

	if (index_count) {
		GL_FlushWorldModelBatch();
	}

	GLM_ExitBatchedPolyRegion();
	return;
}

static void GL_FlushWorldModelBatch(void)
{
	int i;
	GLuint last_vao = 0;
	GLuint last_array = 0;
	qbool was_worldmodel = 0;
	int draw_pos = 0;
	extern glm_vbo_t vbo_brushElements;

	if (!batch_count) {
		return;
	}

	GL_UpdateUBO(&ubo_worldcvars, sizeof(world), &world);
	GL_UpdateVBO(&vbo_brushElements, sizeof(modelIndexes[0]) * index_count, modelIndexes);
	GL_UpdateVBO(&vbo_worldIndirectDraw, sizeof(worldmodel_requests[0]) * batch_count, &worldmodel_requests);

	if (true) {
		glMultiDrawElementsIndirect(
			GL_TRIANGLE_STRIP,
			GL_UNSIGNED_INT,
			(void*)0,
			batch_count,
			sizeof(worldmodel_requests[0])
		);
	}
	else {
		draw_pos = 0;
		for (i = 0; i < batch_count; ++i) {
			int last = i;
			texture_ref texArray = worldmodel_requests[i].texture_array;

			for (last = i; last < batch_count - 1; ++last) {
				texture_ref next = worldmodel_requests[last + 1].texture_array;
				if (GL_TextureReferenceIsValid(next) && GL_TextureReferenceIsValid(texArray) && !GL_TextureReferenceEqual(next, texArray)) {
					break;
				}
				texArray = next;
			}

			if (GL_TextureReferenceIsValid(texArray)) {
				GL_BindTextureUnit(GL_TEXTURE0 + TEXTURE_UNIT_MATERIAL, GL_TEXTURE_2D_ARRAY, texArray);
			}

			// FIXME: All brush models are in the same VAO, sort this out
			glMultiDrawElementsIndirect(
				GL_TRIANGLE_STRIP,
				GL_UNSIGNED_INT,
				(void*)(i * sizeof(worldmodel_requests[0])),
				last - i + 1,
				sizeof(worldmodel_requests[0])
			);

			i = last;
		}
	}

	batch_count = 0;
	index_count = 0;
	material_samplers = 0;
}

void GLM_DrawWorld(model_t* model)
{
	const qbool use_texture_array = true;

	if (model->texture_array_count) {
		GLM_DrawTexturedWorld(model);
	}
}

void GLM_NewMap(void)
{
}

// FIXME: This a) doesn't work in modern, b) shouldn't be in this module
//draws transparent textures for HL world and nonworld models
void R_DrawAlphaChain(msurface_t* alphachain)
{
	int k;
	msurface_t *s;
	texture_t *t;
	float *v;

	if (!alphachain)
		return;

	GL_AlphaBlendFlags(GL_ALPHATEST_ENABLED);
	for (s = alphachain; s; s = s->texturechain) {
		t = s->texinfo->texture;
		R_RenderDynamicLightmaps(s);

		//bind the world texture
		GL_DisableMultitexture();
		GL_BindTextureUnit(GL_TEXTURE0, GL_TEXTURE_2D, t->gl_texturenum);

		if (gl_mtexable) {
			GLC_MultitextureLightmap(s->lightmaptexturenum);
		}

		glBegin(GL_POLYGON);
		v = s->polys->verts[0];
		for (k = 0; k < s->polys->numverts; k++, v += VERTEXSIZE) {
			if (gl_mtexable) {
				qglMultiTexCoord2f(GL_TEXTURE0, v[3], v[4]);
				qglMultiTexCoord2f(GL_TEXTURE1, v[5], v[6]);
			}
			else {
				glTexCoord2f(v[3], v[4]);
			}
			glVertex3fv(v);
		}
		glEnd();
	}

	alphachain = NULL;

	// FIXME: GL_ResetState()
	GL_AlphaBlendFlags(GL_ALPHATEST_DISABLED);
	GL_DisableMultitexture();
	GL_TextureEnvMode(GL_REPLACE);
}
