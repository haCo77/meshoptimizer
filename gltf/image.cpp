// This file is part of gltfpack; see gltfpack.h for version/license details
#include "gltfpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static const char* kMimeTypes[][2] = {
    {"image/jpeg", ".jpg"},
    {"image/jpeg", ".jpeg"},
    {"image/png", ".png"},
};

void analyzeImages(cgltf_data* data, std::vector<ImageInfo>& images)
{
	for (size_t i = 0; i < data->materials_count; ++i)
	{
		const cgltf_material& material = data->materials[i];

		if (material.has_pbr_metallic_roughness)
		{
			const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;

			if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image)
				images[pbr.base_color_texture.texture->image - data->images].srgb = true;
		}

		if (material.has_pbr_specular_glossiness)
		{
			const cgltf_pbr_specular_glossiness& pbr = material.pbr_specular_glossiness;

			if (pbr.diffuse_texture.texture && pbr.diffuse_texture.texture->image)
				images[pbr.diffuse_texture.texture->image - data->images].srgb = true;
		}

		if (material.emissive_texture.texture && material.emissive_texture.texture->image)
			images[material.emissive_texture.texture->image - data->images].srgb = true;

		if (material.normal_texture.texture && material.normal_texture.texture->image)
			images[material.normal_texture.texture->image - data->images].normal_map = true;
	}
}

const char* inferMimeType(const char* path)
{
	const char* ext = strrchr(path, '.');
	if (!ext)
		return "";

	std::string extl = ext;
	for (size_t i = 0; i < extl.length(); ++i)
		extl[i] = char(tolower(extl[i]));

	for (size_t i = 0; i < sizeof(kMimeTypes) / sizeof(kMimeTypes[0]); ++i)
		if (extl == kMimeTypes[i][1])
			return kMimeTypes[i][0];

	return "";
}

static const char* mimeExtension(const char* mime_type)
{
	for (size_t i = 0; i < sizeof(kMimeTypes) / sizeof(kMimeTypes[0]); ++i)
		if (strcmp(kMimeTypes[i][0], mime_type) == 0)
			return kMimeTypes[i][1];

	return ".raw";
}

#ifdef __EMSCRIPTEN__
EM_JS(int, execute, (const char* cmd, bool ignore_stdout, bool ignore_stderr), {
	var cp = require('child_process');
	var stdio = [ 'ignore', ignore_stdout ? 'ignore' : 'inherit', ignore_stderr ? 'ignore' : 'inherit' ];
	var ret = cp.spawnSync(UTF8ToString(cmd), [], {shell:true, stdio:stdio });
	return ret.status == null ? 256 : ret.status;
});

EM_JS(const char*, readenv, (const char* name), {
	var val = process.env[UTF8ToString(name)];
	if (!val) return 0;
	var ret = _malloc(lengthBytesUTF8(val) + 1);
	stringToUTF8(val, ret, lengthBytesUTF8(val) + 1);
	return ret;
});
#else
static int execute(const char* cmd_, bool ignore_stdout, bool ignore_stderr)
{
#ifdef _WIN32
	std::string ignore = "nul";
#else
	std::string ignore = "/dev/null";
#endif

	std::string cmd = cmd_;

	if (ignore_stdout)
		(cmd += " >") += ignore;
	if (ignore_stderr)
		(cmd += " 2>") += ignore;

	return system(cmd.c_str());
}

static const char* readenv(const char* name)
{
	return getenv(name);
}
#endif

bool checkBasis(bool verbose)
{
	const char* basisu_path = readenv("BASISU_PATH");
	std::string cmd = basisu_path ? basisu_path : "basisu";

	cmd += " -version";

	int rc = execute(cmd.c_str(), /* ignore_stdout= */ true, /* ignore_stderr= */ true);
	if (verbose)
		printf("%s => %d\n", cmd.c_str(), rc);

	return rc == 0;
}

bool encodeBasis(const std::string& data, const char* mime_type, std::string& result, bool normal_map, bool srgb, int quality, float scale, bool uastc, bool verbose)
{
	(void)scale;

	TempFile temp_input(mimeExtension(mime_type));
	TempFile temp_output(".basis");

	if (!writeFile(temp_input.path.c_str(), data))
		return false;

	const char* basisu_path = readenv("BASISU_PATH");
	std::string cmd = basisu_path ? basisu_path : "basisu";

	char ql[16];
	sprintf(ql, "%d", (quality * 255 + 50) / 100);

	cmd += " -q ";
	cmd += ql;

	cmd += " -mipmap";

	if (normal_map)
	{
		cmd += " -normal_map";
		// for optimal quality we should specify seperate_rg_to_color_alpha but this requires renderer awareness
	}
	else if (!srgb)
	{
		cmd += " -linear";
	}

	if (uastc)
	{
		cmd += " -uastc";
	}

	cmd += " -file ";
	cmd += temp_input.path;
	cmd += " -output_file ";
	cmd += temp_output.path;

	int rc = execute(cmd.c_str(), /* ignore_stdout= */ true, /* ignore_stderr= */ false);
	if (verbose)
		printf("%s => %d\n", cmd.c_str(), rc);

	return rc == 0 && readFile(temp_output.path.c_str(), result);
}

bool checkKtx(bool verbose)
{
	const char* toktx_path = readenv("TOKTX_PATH");
	std::string cmd = toktx_path ? toktx_path : "toktx";

	cmd += " --version";

	int rc = execute(cmd.c_str(), /* ignore_stdout= */ true, /* ignore_stderr= */ true);
	if (verbose)
		printf("%s => %d\n", cmd.c_str(), rc);

	return rc == 0;
}

bool encodeKtx(const std::string& data, const char* mime_type, std::string& result, bool normal_map, bool srgb, int quality, float scale, bool uastc, bool verbose)
{
	TempFile temp_input(mimeExtension(mime_type));
	TempFile temp_output(".ktx2");

	if (!writeFile(temp_input.path.c_str(), data))
		return false;

	const char* toktx_path = readenv("TOKTX_PATH");
	std::string cmd = toktx_path ? toktx_path : "toktx";

	cmd += " --2d";
	cmd += " --t2";

	cmd += " --automipmap";

	if (scale < 1)
	{
		char sl[128];
		sprintf(sl, "%g", scale);

		cmd += " --scale ";
		cmd += sl;
	}

	if (uastc)
	{
		cmd += " --uastc 2";
	}
	else
	{
		char ql[16];
		sprintf(ql, "%d", (quality * 255 + 50) / 100);

		cmd += " --bcmp";
		cmd += " --qlevel ";
		cmd += ql;

		// for optimal quality we should specify separate_rg_to_color_alpha but this requires renderer awareness
		if (normal_map)
			cmd += " --normal_map";
	}

	if (srgb)
		cmd += " --srgb";
	else
		cmd += " --linear";

	cmd += " ";
	cmd += temp_output.path;
	cmd += " ";
	cmd += temp_input.path;

	int rc = execute(cmd.c_str(), /* ignore_stdout= */ false, /* ignore_stderr= */ false);
	if (verbose)
		printf("%s => %d\n", cmd.c_str(), rc);

	return rc == 0 && readFile(temp_output.path.c_str(), result);
}
