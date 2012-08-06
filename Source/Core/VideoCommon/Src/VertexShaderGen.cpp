// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include <math.h>
#include <locale.h>

#include "NativeVertexFormat.h"

#include "BPMemory.h"
#include "CPMemory.h"
#include "LightingShaderGen.h"
#include "VertexShaderGen.h"
#include "VideoConfig.h"

static char text[16768];

enum GenOutput
{
	GO_ShaderCode,
	GO_ShaderUid,
};
// TODO: Check if something goes wrong if the cached shaders used pixel lighting but it's disabled later??
template<class T>
void GenerateVSOutputStruct(T& object, u32 components, API_TYPE api_type)
{
	object.Write("struct VS_OUTPUT {\n");
	object.Write("  float4 pos : POSITION;\n");
	object.Write("  float4 colors_0 : COLOR0;\n");
	object.Write("  float4 colors_1 : COLOR1;\n");

	if (xfregs.numTexGen.numTexGens < 7)
	{
		for (unsigned int i = 0; i < xfregs.numTexGen.numTexGens; ++i)
			object.Write("  float3 tex%d : TEXCOORD%d;\n", i, i);

		object.Write("  float4 clipPos : TEXCOORD%d;\n", xfregs.numTexGen.numTexGens);
///		if(g_ActiveConfig.bEnablePixelLighting && g_ActiveConfig.backend_info.bSupportsPixelLighting)
///			object.Write("  float4 Normal : TEXCOORD%d;\n", xfregs.numTexGen.numTexGens + 1);
	}
	else
	{
		// clip position is in w of first 4 texcoords
///		if(g_ActiveConfig.bEnablePixelLighting && g_ActiveConfig.backend_info.bSupportsPixelLighting)
///		{
///			for (int i = 0; i < 8; ++i)
///				object.Write("  float4 tex%d : TEXCOORD%d;\n", i, i);
///		}
///		else
		{
			for (unsigned int i = 0; i < xfregs.numTexGen.numTexGens; ++i)
				object.Write("  float%d tex%d : TEXCOORD%d;\n", i < 4 ? 4 : 3 , i, i);
		}
	}
	object.Write("};\n");
}

template<class T, GenOutput type>
void _GenerateLightShader(T& object, int index, int litchan_index, const char* lightsName, int coloralpha)
{
#define SetUidField(name, value) if (type == GO_ShaderUid) { object.GetUidData().name = value; };
	const LitChannel& chan = (litchan_index > 1) ? xfregs.alpha[litchan_index-2] : xfregs.color[litchan_index];
	const char* swizzle = "xyzw";
	if (coloralpha == 1 ) swizzle = "xyz";
	else if (coloralpha == 2 ) swizzle = "w";

	SetUidField(lit_chans[litchan_index].attnfunc, chan.attnfunc);
	SetUidField(lit_chans[litchan_index].diffusefunc, chan.diffusefunc);
	if (!(chan.attnfunc & 1)) {
		// atten disabled
		switch (chan.diffusefunc) {
			case LIGHTDIF_NONE:
				object.Write("lacc.%s += %s.lights[%d].col.%s;\n", swizzle, lightsName, index, swizzle);
				break;
			case LIGHTDIF_SIGN:
			case LIGHTDIF_CLAMP:
				object.Write("ldir = normalize(%s.lights[%d].pos.xyz - pos.xyz);\n", lightsName, index);
				object.Write("lacc.%s += %sdot(ldir, _norm0)) * %s.lights[%d].col.%s;\n",
					swizzle, chan.diffusefunc != LIGHTDIF_SIGN ? "max(0.0f," :"(", lightsName, index, swizzle);
				break;
			default: _assert_(0);
		}
	}
	else { // spec and spot

		if (chan.attnfunc == 3)
		{ // spot
			object.Write("ldir = %s.lights[%d].pos.xyz - pos.xyz;\n", lightsName, index);
			object.Write("dist2 = dot(ldir, ldir);\n"
						"dist = sqrt(dist2);\n"
						"ldir = ldir / dist;\n"
						"attn = max(0.0f, dot(ldir, %s.lights[%d].dir.xyz));\n", lightsName, index);
			object.Write("attn = max(0.0f, dot(%s.lights[%d].cosatt.xyz, float3(1.0f, attn, attn*attn))) / dot(%s.lights[%d].distatt.xyz, float3(1.0f,dist,dist2));\n", lightsName, index, lightsName, index);
		}
		else if (chan.attnfunc == 1)
		{ // specular
			object.Write("ldir = normalize(%s.lights[%d].pos.xyz);\n", lightsName, index);
			object.Write("attn = (dot(_norm0,ldir) >= 0.0f) ? max(0.0f, dot(_norm0, %s.lights[%d].dir.xyz)) : 0.0f;\n", lightsName, index);
			object.Write("attn = max(0.0f, dot(%s.lights[%d].cosatt.xyz, float3(1,attn,attn*attn))) / dot(%s.lights[%d].distatt.xyz, float3(1,attn,attn*attn));\n", lightsName, index, lightsName, index);
		}

		switch (chan.diffusefunc)
		{
			case LIGHTDIF_NONE:
				object.Write("lacc.%s += attn * %s.lights[%d].col.%s;\n", swizzle, lightsName, index, swizzle);
				break;
			case LIGHTDIF_SIGN:
			case LIGHTDIF_CLAMP:
				object.Write("lacc.%s += attn * %sdot(ldir, _norm0)) * %s.lights[%d].col.%s;\n",
					swizzle,
					chan.diffusefunc != LIGHTDIF_SIGN ? "max(0.0f," :"(",
					lightsName,
					index,
					swizzle);
				break;
			default: _assert_(0);
		}
	}
	object.Write("\n");
}

// vertex shader
// lights/colors
// materials name is I_MATERIALS in vs and I_PMATERIALS in ps
// inColorName is color in vs and colors_ in ps
// dest is o.colors_ in vs and colors_ in ps
template<class T, GenOutput type>
void _GenerateLightingShader(T& object, int components, const char* materialsName, const char* lightsName, const char* inColorName, const char* dest)
{
	for (unsigned int j = 0; j < xfregs.numChan.numColorChans; j++)
	{
		const LitChannel& color = xfregs.color[j];
		const LitChannel& alpha = xfregs.alpha[j];

		object.Write("{\n");

		SetUidField(lit_chans[j].matsource, xfregs.color[j].matsource);
		if (color.matsource) {// from vertex
			if (components & (VB_HAS_COL0 << j))
				object.Write("mat = %s%d;\n", inColorName, j);
			else if (components & VB_HAS_COL0)
				object.Write("mat = %s0;\n", inColorName);
			else
				object.Write("mat = float4(1.0f, 1.0f, 1.0f, 1.0f);\n");
		}
		else // from color
			object.Write("mat = %s.C%d;\n", materialsName, j+2);

		SetUidField(lit_chans[j].enablelighting, xfregs.color[j].enablelighting);
		if (color.enablelighting) {
			SetUidField(lit_chans[j].ambsource, xfregs.color[j].ambsource);
			if (color.ambsource) { // from vertex
				if (components & (VB_HAS_COL0<<j) )
					object.Write("lacc = %s%d;\n", inColorName, j);
				else if (components & VB_HAS_COL0 )
					object.Write("lacc = %s0;\n", inColorName);
				else
					object.Write("lacc = float4(0.0f, 0.0f, 0.0f, 0.0f);\n");
			}
			else // from color
				object.Write("lacc = %s.C%d;\n", materialsName, j);
		}
		else
		{
			object.Write("lacc = float4(1.0f, 1.0f, 1.0f, 1.0f);\n");
		}

		// check if alpha is different
		SetUidField(lit_chans[j+2].matsource, xfregs.alpha[j].matsource);
		if (alpha.matsource != color.matsource) {
			if (alpha.matsource) {// from vertex
				if (components & (VB_HAS_COL0<<j))
					object.Write("mat.w = %s%d.w;\n", inColorName, j);
				else if (components & VB_HAS_COL0)
					object.Write("mat.w = %s0.w;\n", inColorName);
				else object.Write("mat.w = 1.0f;\n");
			}
			else // from color
				object.Write("mat.w = %s.C%d.w;\n", materialsName, j+2);
		}

		SetUidField(lit_chans[j+2].enablelighting, xfregs.alpha[j].enablelighting);
		if (alpha.enablelighting)
		{
			SetUidField(lit_chans[j+2].ambsource, xfregs.alpha[j].ambsource);
			if (alpha.ambsource) {// from vertex
				if (components & (VB_HAS_COL0<<j) )
					object.Write("lacc.w = %s%d.w;\n", inColorName, j);
				else if (components & VB_HAS_COL0 )
					object.Write("lacc.w = %s0.w;\n", inColorName);
				else
					object.Write("lacc.w = 0.0f;\n");
			}
			else // from color
				object.Write("lacc.w = %s.C%d.w;\n", materialsName, j);
		}
		else
		{
			object.Write("lacc.w = 1.0f;\n");
		}

		if(color.enablelighting && alpha.enablelighting)
		{
			// both have lighting, test if they use the same lights
			int mask = 0;
			SetUidField(lit_chans[j].attnfunc, color.attnfunc);
			SetUidField(lit_chans[j+2].attnfunc, alpha.attnfunc);
			SetUidField(lit_chans[j].diffusefunc, color.diffusefunc);
			SetUidField(lit_chans[j+2].diffusefunc, alpha.diffusefunc);
			SetUidField(lit_chans[j].light_mask, color.GetFullLightMask());
			SetUidField(lit_chans[j+2].light_mask, alpha.GetFullLightMask());
			if(color.lightparams == alpha.lightparams)
			{
				mask = color.GetFullLightMask() & alpha.GetFullLightMask();
				if(mask)
				{
					for (int i = 0; i < 8; ++i)
					{
						if (mask & (1<<i))
						{
							_GenerateLightShader<T,type>(object, i, j, lightsName, 3);
						}
					}
				}
			}

			// no shared lights
			for (int i = 0; i < 8; ++i)
			{
				if (!(mask&(1<<i)) && (color.GetFullLightMask() & (1<<i)))
					_GenerateLightShader<T,type>(object, i, j, lightsName, 1);
				if (!(mask&(1<<i)) && (alpha.GetFullLightMask() & (1<<i)))
					_GenerateLightShader<T,type>(object, i, j+2, lightsName, 2);
			}
		}
		else if (color.enablelighting || alpha.enablelighting)
		{
			// lights are disabled on one channel so process only the active ones
			const LitChannel& workingchannel = color.enablelighting ? color : alpha;
			const int lit_index = color.enablelighting ? j : (j+2);
			int coloralpha = color.enablelighting ? 1 : 2;

			SetUidField(lit_chans[lit_index].light_mask, workingchannel.GetFullLightMask());
			for (int i = 0; i < 8; ++i)
			{
				if (workingchannel.GetFullLightMask() & (1<<i))
					_GenerateLightShader<T,type>(object, i, lit_index, lightsName, coloralpha);
			}
		}
		object.Write("%s%d = mat * saturate(lacc);\n", dest, j);
		object.Write("}\n");
	}
}

// TODO: Problem: this one uses copy constructors or sth for uids when returning...
template<class T, GenOutput type>
void GenerateVertexShader(T& out, u32 components, API_TYPE api_type)
{
#undef SetUidField
#define SetUidField(name, value) if (type == GO_ShaderUid) {out.GetUidData().name = value; };

	if (type == GO_ShaderCode)
	{
		out.SetBuffer(text);
		setlocale(LC_NUMERIC, "C"); // Reset locale for compilation
	}

	///	text[sizeof(text) - 1] = 0x7C;  // canary

	bool is_d3d = (api_type & API_D3D9 || api_type == API_D3D11);
	u32 lightMask = 0;
	if (xfregs.numChan.numColorChans > 0)
		lightMask |= xfregs.color[0].GetFullLightMask() | xfregs.alpha[0].GetFullLightMask();
	if (xfregs.numChan.numColorChans > 1)
		lightMask |= xfregs.color[1].GetFullLightMask() | xfregs.alpha[1].GetFullLightMask();

	out.Write("//Vertex Shader: comp:%x, \n", components);
	out.Write("typedef struct { float4 T0, T1, T2; float4 N0, N1, N2; } s_" I_POSNORMALMATRIX";\n"
			"typedef struct { float4 t; } FLT4;\n"
			"typedef struct { FLT4 T[24]; } s_" I_TEXMATRICES";\n"
			"typedef struct { FLT4 T[64]; } s_" I_TRANSFORMMATRICES";\n"
			"typedef struct { FLT4 T[32]; } s_" I_NORMALMATRICES";\n"
			"typedef struct { FLT4 T[64]; } s_" I_POSTTRANSFORMMATRICES";\n"
			"typedef struct { float4 col; float4 cosatt; float4 distatt; float4 pos; float4 dir; } Light;\n"
			"typedef struct { Light lights[8]; } s_" I_LIGHTS";\n"
			"typedef struct { float4 C0, C1, C2, C3; } s_" I_MATERIALS";\n"
			"typedef struct { float4 T0, T1, T2, T3; } s_" I_PROJECTION";\n"
			);

///	p = GenerateVSOutputStruct(p, components, api_type);
	GenerateVSOutputStruct(out, components, api_type);

	// uniforms

	out.Write("uniform s_" I_TRANSFORMMATRICES" " I_TRANSFORMMATRICES" : register(c%d);\n", C_TRANSFORMMATRICES);
	out.Write("uniform s_" I_TEXMATRICES" " I_TEXMATRICES" : register(c%d);\n", C_TEXMATRICES);
	out.Write("uniform s_" I_NORMALMATRICES" " I_NORMALMATRICES" : register(c%d);\n", C_NORMALMATRICES);
	out.Write("uniform s_" I_POSNORMALMATRIX" " I_POSNORMALMATRIX" : register(c%d);\n", C_POSNORMALMATRIX);
	out.Write("uniform s_" I_POSTTRANSFORMMATRICES" " I_POSTTRANSFORMMATRICES" : register(c%d);\n", C_POSTTRANSFORMMATRICES);
	out.Write("uniform s_" I_LIGHTS" " I_LIGHTS" : register(c%d);\n", C_LIGHTS);
	out.Write("uniform s_" I_MATERIALS" " I_MATERIALS" : register(c%d);\n", C_MATERIALS);
	out.Write("uniform s_" I_PROJECTION" " I_PROJECTION" : register(c%d);\n", C_PROJECTION);
	out.Write("uniform float4 " I_DEPTHPARAMS" : register(c%d);\n", C_DEPTHPARAMS);

	out.Write("VS_OUTPUT main(\n");

	SetUidField(numTexGens, xfregs.numTexGen.numTexGens);
	SetUidField(components, components);
	// inputs
	if (components & VB_HAS_NRM0)
		out.Write("  float3 rawnorm0 : NORMAL0,\n");
	if (components & VB_HAS_NRM1)
	{
		if (is_d3d)
			out.Write("  float3 rawnorm1 : NORMAL1,\n");
		else
			out.Write("  float3 rawnorm1 : ATTR%d,\n", SHADER_NORM1_ATTRIB);
	}
	if (components & VB_HAS_NRM2)
	{
		if (is_d3d)
			out.Write("  float3 rawnorm2 : NORMAL2,\n");
		else
			out.Write("  float3 rawnorm2 : ATTR%d,\n", SHADER_NORM2_ATTRIB);
	}
	if (components & VB_HAS_COL0)
		out.Write("  float4 color0 : COLOR0,\n");
	if (components & VB_HAS_COL1)
		out.Write("  float4 color1 : COLOR1,\n");
	for (int i = 0; i < 8; ++i) {
		u32 hastexmtx = (components & (VB_HAS_TEXMTXIDX0<<i));
		if ((components & (VB_HAS_UV0<<i)) || hastexmtx)
			out.Write("  float%d tex%d : TEXCOORD%d,\n", hastexmtx ? 3 : 2, i, i);
	}
	if (components & VB_HAS_POSMTXIDX) {
		if (is_d3d)
		{
			out.Write("  float4 blend_indices : BLENDINDICES,\n");
		}
		else
			out.Write("  float fposmtx : ATTR%d,\n", SHADER_POSMTX_ATTRIB);
	}
	out.Write("  float4 rawpos : POSITION) {\n");
	out.Write("VS_OUTPUT o;\n");

	// transforms
	if (components & VB_HAS_POSMTXIDX)
	{
		if (api_type & API_D3D9)
		{
			out.Write("int4 indices = D3DCOLORtoUBYTE4(blend_indices);\n");
			out.Write("int posmtx = indices.x;\n");
		}
		else if (api_type == API_D3D11)
		{
			out.Write("int posmtx = blend_indices.x * 255.0f;\n");
		}
		else
		{
			out.Write("int posmtx = fposmtx;\n");
		}

		out.Write("float4 pos = float4(dot(" I_TRANSFORMMATRICES".T[posmtx].t, rawpos), dot(" I_TRANSFORMMATRICES".T[posmtx+1].t, rawpos), dot(" I_TRANSFORMMATRICES".T[posmtx+2].t, rawpos), 1);\n");

		if (components & VB_HAS_NRMALL) {
			out.Write("int normidx = posmtx >= 32 ? (posmtx-32) : posmtx;\n");
			out.Write("float3 N0 = " I_NORMALMATRICES".T[normidx].t.xyz, N1 = " I_NORMALMATRICES".T[normidx+1].t.xyz, N2 = " I_NORMALMATRICES".T[normidx+2].t.xyz;\n");
		}

		if (components & VB_HAS_NRM0)
			out.Write("float3 _norm0 = normalize(float3(dot(N0, rawnorm0), dot(N1, rawnorm0), dot(N2, rawnorm0)));\n");
		if (components & VB_HAS_NRM1)
			out.Write("float3 _norm1 = float3(dot(N0, rawnorm1), dot(N1, rawnorm1), dot(N2, rawnorm1));\n");
		if (components & VB_HAS_NRM2)
			out.Write("float3 _norm2 = float3(dot(N0, rawnorm2), dot(N1, rawnorm2), dot(N2, rawnorm2));\n");
	}
	else
	{
		out.Write("float4 pos = float4(dot(" I_POSNORMALMATRIX".T0, rawpos), dot(" I_POSNORMALMATRIX".T1, rawpos), dot(" I_POSNORMALMATRIX".T2, rawpos), 1.0f);\n");
		if (components & VB_HAS_NRM0)
			out.Write("float3 _norm0 = normalize(float3(dot(" I_POSNORMALMATRIX".N0.xyz, rawnorm0), dot(" I_POSNORMALMATRIX".N1.xyz, rawnorm0), dot(" I_POSNORMALMATRIX".N2.xyz, rawnorm0)));\n");
		if (components & VB_HAS_NRM1)
			out.Write("float3 _norm1 = float3(dot(" I_POSNORMALMATRIX".N0.xyz, rawnorm1), dot(" I_POSNORMALMATRIX".N1.xyz, rawnorm1), dot(" I_POSNORMALMATRIX".N2.xyz, rawnorm1));\n");
		if (components & VB_HAS_NRM2)
			out.Write("float3 _norm2 = float3(dot(" I_POSNORMALMATRIX".N0.xyz, rawnorm2), dot(" I_POSNORMALMATRIX".N1.xyz, rawnorm2), dot(" I_POSNORMALMATRIX".N2.xyz, rawnorm2));\n");
	}

	if (!(components & VB_HAS_NRM0))
		out.Write("float3 _norm0 = float3(0.0f, 0.0f, 0.0f);\n");



	out.Write("o.pos = float4(dot(" I_PROJECTION".T0, pos), dot(" I_PROJECTION".T1, pos), dot(" I_PROJECTION".T2, pos), dot(" I_PROJECTION".T3, pos));\n");

	out.Write("float4 mat, lacc;\n"
			"float3 ldir, h;\n"
			"float dist, dist2, attn;\n");

	SetUidField(numColorChans, xfregs.numChan.numColorChans);
	if(xfregs.numChan.numColorChans == 0)
	{
		if (components & VB_HAS_COL0)
			out.Write("o.colors_0 = color0;\n");
		else
			out.Write("o.colors_0 = float4(1.0f, 1.0f, 1.0f, 1.0f);\n");
	}

	// TODO: This probably isn't necessary if pixel lighting is enabled.
	_GenerateLightingShader<T,type>(out, components, I_MATERIALS, I_LIGHTS, "color", "o.colors_");

	if(xfregs.numChan.numColorChans < 2)
	{
		if (components & VB_HAS_COL1)
			out.Write("o.colors_1 = color1;\n");
		else
			out.Write("o.colors_1 = o.colors_0;\n");
	}
	// special case if only pos and tex coord 0 and tex coord input is AB11
	// donko - this has caused problems in some games. removed for now.
	bool texGenSpecialCase = false;
	/*bool texGenSpecialCase =
		((g_VtxDesc.Hex & 0x60600L) == g_VtxDesc.Hex) && // only pos and tex coord 0
		(g_VtxDesc.Tex0Coord != NOT_PRESENT) &&
		(xfregs.texcoords[0].texmtxinfo.inputform == XF_TEXINPUT_AB11);
		*/

	// transform texcoords
	out.Write("float4 coord = float4(0.0f, 0.0f, 1.0f, 1.0f);\n");
	for (unsigned int i = 0; i < xfregs.numTexGen.numTexGens; ++i) {
		TexMtxInfo& texinfo = xfregs.texMtxInfo[i];

		out.Write("{\n");
		out.Write("coord = float4(0.0f, 0.0f, 1.0f, 1.0f);\n");
		SetUidField(texMtxInfo[i].sourcerow, xfregs.texMtxInfo[i].sourcerow);
		switch (texinfo.sourcerow) {
		case XF_SRCGEOM_INROW:
			_assert_( texinfo.inputform == XF_TEXINPUT_ABC1 );
			out.Write("coord = rawpos;\n"); // pos.w is 1
			break;
		case XF_SRCNORMAL_INROW:
			if (components & VB_HAS_NRM0) {
				_assert_( texinfo.inputform == XF_TEXINPUT_ABC1 );
				out.Write("coord = float4(rawnorm0.xyz, 1.0f);\n");
			}
			break;
		case XF_SRCCOLORS_INROW:
			_assert_( texinfo.texgentype == XF_TEXGEN_COLOR_STRGBC0 || texinfo.texgentype == XF_TEXGEN_COLOR_STRGBC1 );
			break;
		case XF_SRCBINORMAL_T_INROW:
			if (components & VB_HAS_NRM1) {
				_assert_( texinfo.inputform == XF_TEXINPUT_ABC1 );
				out.Write("coord = float4(rawnorm1.xyz, 1.0f);\n");
			}
			break;
		case XF_SRCBINORMAL_B_INROW:
			if (components & VB_HAS_NRM2) {
				_assert_( texinfo.inputform == XF_TEXINPUT_ABC1 );
				out.Write("coord = float4(rawnorm2.xyz, 1.0f);\n");
			}
			break;
		default:
			_assert_(texinfo.sourcerow <= XF_SRCTEX7_INROW);
			if (components & (VB_HAS_UV0<<(texinfo.sourcerow - XF_SRCTEX0_INROW)) )
				out.Write("coord = float4(tex%d.x, tex%d.y, 1.0f, 1.0f);\n", texinfo.sourcerow - XF_SRCTEX0_INROW, texinfo.sourcerow - XF_SRCTEX0_INROW);
			break;
		}

		// first transformation
		SetUidField(texMtxInfo[i].texgentype, xfregs.texMtxInfo[i].texgentype);
		switch (texinfo.texgentype) {
			case XF_TEXGEN_EMBOSS_MAP: // calculate tex coords into bump map

				if (components & (VB_HAS_NRM1|VB_HAS_NRM2)) {
					// transform the light dir into tangent space
					SetUidField(texMtxInfo[i].embosslightshift, xfregs.texMtxInfo[i].embosslightshift);
					SetUidField(texMtxInfo[i].embosssourceshift, xfregs.texMtxInfo[i].embosssourceshift);
					out.Write("ldir = normalize(" I_LIGHTS".lights[%d].pos.xyz - pos.xyz);\n", texinfo.embosslightshift);
					out.Write("o.tex%d.xyz = o.tex%d.xyz + float3(dot(ldir, _norm1), dot(ldir, _norm2), 0.0f);\n", i, texinfo.embosssourceshift);
				}
				else
				{
					_assert_(0); // should have normals
					SetUidField(texMtxInfo[i].embosssourceshift, xfregs.texMtxInfo[i].embosssourceshift);
					out.Write("o.tex%d.xyz = o.tex%d.xyz;\n", i, texinfo.embosssourceshift);
				}

				break;
			case XF_TEXGEN_COLOR_STRGBC0:
				_assert_(texinfo.sourcerow == XF_SRCCOLORS_INROW);
				out.Write("o.tex%d.xyz = float3(o.colors_0.x, o.colors_0.y, 1);\n", i);
				break;
			case XF_TEXGEN_COLOR_STRGBC1:
				_assert_(texinfo.sourcerow == XF_SRCCOLORS_INROW);
				out.Write("o.tex%d.xyz = float3(o.colors_1.x, o.colors_1.y, 1);\n", i);
				break;
			case XF_TEXGEN_REGULAR:
			default:
				SetUidField(texMtxInfo[i].projection, xfregs.texMtxInfo[i].projection);
				if (components & (VB_HAS_TEXMTXIDX0<<i)) {
					if (texinfo.projection == XF_TEXPROJ_STQ)
						out.Write("o.tex%d.xyz = float3(dot(coord, " I_TRANSFORMMATRICES".T[tex%d.z].t), dot(coord, " I_TRANSFORMMATRICES".T[tex%d.z+1].t), dot(coord, " I_TRANSFORMMATRICES".T[tex%d.z+2].t));\n", i, i, i, i);
					else {
						out.Write("o.tex%d.xyz = float3(dot(coord, " I_TRANSFORMMATRICES".T[tex%d.z].t), dot(coord, " I_TRANSFORMMATRICES".T[tex%d.z+1].t), 1);\n", i, i, i);
					}
				}
				else {
					if (texinfo.projection == XF_TEXPROJ_STQ)
						out.Write("o.tex%d.xyz = float3(dot(coord, " I_TEXMATRICES".T[%d].t), dot(coord, " I_TEXMATRICES".T[%d].t), dot(coord, " I_TEXMATRICES".T[%d].t));\n", i, 3*i, 3*i+1, 3*i+2);
					else
						out.Write("o.tex%d.xyz = float3(dot(coord, " I_TEXMATRICES".T[%d].t), dot(coord, " I_TEXMATRICES".T[%d].t), 1);\n", i, 3*i, 3*i+1);
				}
				break;
		}

		SetUidField(dualTexTrans.enabled, xfregs.dualTexTrans.enabled);
		if (xfregs.dualTexTrans.enabled && texinfo.texgentype == XF_TEXGEN_REGULAR) { // only works for regular tex gen types?
			const PostMtxInfo& postInfo = xfregs.postMtxInfo[i];

			SetUidField(postMtxInfo[i].index, xfregs.postMtxInfo[i].index);
			int postidx = postInfo.index;
			out.Write("float4 P0 = " I_POSTTRANSFORMMATRICES".T[%d].t;\n"
					"float4 P1 = " I_POSTTRANSFORMMATRICES".T[%d].t;\n"
					"float4 P2 = " I_POSTTRANSFORMMATRICES".T[%d].t;\n",
					postidx&0x3f, (postidx+1)&0x3f, (postidx+2)&0x3f);

			if (texGenSpecialCase) {
				// no normalization
				// q of input is 1
				// q of output is unknown

				// multiply by postmatrix
				out.Write("o.tex%d.xyz = float3(dot(P0.xy, o.tex%d.xy) + P0.z + P0.w, dot(P1.xy, o.tex%d.xy) + P1.z + P1.w, 0.0f);\n", i, i, i);
			}
			else
			{
				SetUidField(postMtxInfo[i].normalize, xfregs.postMtxInfo[i].normalize);
				if (postInfo.normalize)
					out.Write("o.tex%d.xyz = normalize(o.tex%d.xyz);\n", i, i);

				// multiply by postmatrix
				out.Write("o.tex%d.xyz = float3(dot(P0.xyz, o.tex%d.xyz) + P0.w, dot(P1.xyz, o.tex%d.xyz) + P1.w, dot(P2.xyz, o.tex%d.xyz) + P2.w);\n", i, i, i, i);
			}
		}

		out.Write("}\n");
	}

	// clipPos/w needs to be done in pixel shader, not here
	if (xfregs.numTexGen.numTexGens < 7) {
		out.Write("o.clipPos = float4(pos.x,pos.y,o.pos.z,o.pos.w);\n");
	} else {
		out.Write("o.tex0.w = pos.x;\n");
		out.Write("o.tex1.w = pos.y;\n");
		out.Write("o.tex2.w = o.pos.z;\n");
		out.Write("o.tex3.w = o.pos.w;\n");
	}

/*	if(g_ActiveConfig.bEnablePixelLighting && g_ActiveConfig.backend_info.bSupportsPixelLighting)
	{
		if (xfregs.numTexGen.numTexGens < 7) {
			out.Write("o.Normal = float4(_norm0.x,_norm0.y,_norm0.z,pos.z);\n");
		} else {
			out.Write("o.tex4.w = _norm0.x;\n");
			out.Write("o.tex5.w = _norm0.y;\n");
			out.Write("o.tex6.w = _norm0.z;\n");
			if (xfregs.numTexGen.numTexGens < 8)
				out.Write("o.tex7 = pos.xyzz;\n");
			else
				out.Write("o.tex7.w = pos.z;\n");
		}
		if (components & VB_HAS_COL0)
			out.Write("o.colors_0 = color0;\n");

		if (components & VB_HAS_COL1)
			out.Write("o.colors_1 = color1;\n");
	}*/

	//write the true depth value, if the game uses depth textures pixel shaders will override with the correct values
	//if not early z culling will improve speed
	// TODO: Can probably be dropped?
	if (is_d3d)
	{
		out.Write("o.pos.z = " I_DEPTHPARAMS".x * o.pos.w + o.pos.z * " I_DEPTHPARAMS".y;\n");
	}
	else
	{
	    // this results in a scale from -1..0 to -1..1 after perspective
	    // divide
	    out.Write("o.pos.z = o.pos.w + o.pos.z * 2.0f;\n");

	    // Sonic Unleashed puts its final rendering at the near or
	    // far plane of the viewing frustrum(actually box, they use
	    // orthogonal projection for that), and we end up putting it
	    // just beyond, and the rendering gets clipped away. (The
	    // primitive gets dropped)
	    out.Write("o.pos.z = o.pos.z * 1048575.0f/1048576.0f;\n");

	    // the next steps of the OGL pipeline are:
	    // (x_c,y_c,z_c,w_c) = o.pos  //switch to OGL spec terminology
	    // clipping to -w_c <= (x_c,y_c,z_c) <= w_c
	    // (x_d,y_d,z_d) = (x_c,y_c,z_c)/w_c//perspective divide
	    // z_w = (f-n)/2*z_d + (n+f)/2
	    // z_w now contains the value to go to the 0..1 depth buffer

	    //trying to get the correct semantic while not using glDepthRange
	    //seems to get rather complicated
	}

	if (api_type & API_D3D9)
	{
		// D3D9 is addressing pixel centers instead of pixel boundaries in clip space.
		// Thus we need to offset the final position by half a pixel
		out.Write("o.pos = o.pos + float4(" I_DEPTHPARAMS".z, " I_DEPTHPARAMS".w, 0.f, 0.f);\n");
	}

	out.Write("return o;\n}\n");


///	if (text[sizeof(text) - 1] != 0x7C)
///		PanicAlert("VertexShader generator - buffer too small, canary has been eaten!");
	if (type == GO_ShaderCode)
		setlocale(LC_NUMERIC, ""); // restore locale
}

void GetVertexShaderUid(VertexShaderUid& object, u32 components, API_TYPE api_type)
{
	GenerateVertexShader<VertexShaderUid, GO_ShaderUid>(object, components, api_type);
}

void GenerateVertexShaderCode(VertexShaderCode& object, u32 components, API_TYPE api_type)
{
	GenerateVertexShader<VertexShaderCode, GO_ShaderCode>(object, components, api_type);
}
