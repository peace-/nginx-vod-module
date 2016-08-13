#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/hls/hls_muxer.h"

// content types
static u_char mpeg_ts_content_type[] = "";
static u_char m3u8_content_type[] = "application/octet-stream";
static u_char encryption_key_content_type[] = "application/octet-stream";

static const u_char ts_file_ext[] = ".qts";
static const u_char m3u8_file_ext[] = ".qm3u8";
static const u_char key_file_ext[] = ".key";

// constants
static ngx_str_t empty_string = ngx_null_string;

ngx_conf_enum_t  hls_encryption_methods[] = {
	{ ngx_string("none"), HLS_ENC_NONE },
	{ ngx_string("aes-128"), HLS_ENC_AES_128 },
	{ ngx_string("sample-aes"), HLS_ENC_SAMPLE_AES },
	{ ngx_null_string, 0 }
};

static void
ngx_http_vod_hls_init_encryption_iv(u_char* iv, uint32_t segment_index)
{
	u_char* p;

	// the IV is the segment index in big endian
	vod_memzero(iv, AES_BLOCK_SIZE - sizeof(uint32_t));
	segment_index++;
	p = iv + AES_BLOCK_SIZE - sizeof(uint32_t);
	*p++ = (u_char)(segment_index >> 24);
	*p++ = (u_char)(segment_index >> 16);
	*p++ = (u_char)(segment_index >> 8);
	*p++ = (u_char)(segment_index);
}

static void
ngx_http_vod_hls_init_encryption_params(
	hls_encryption_params_t* encryption_params,
	ngx_http_vod_submodule_context_t* submodule_context,
	u_char* iv)
{
	encryption_params->type = submodule_context->conf->hls.encryption_method;
	if (encryption_params->type == HLS_ENC_NONE)
	{
		return;
	}

	ngx_http_vod_hls_init_encryption_iv(iv, submodule_context->request_params.segment_index);
	encryption_params->key = submodule_context->request_params.suburis->encryption_key;
	encryption_params->iv = iv;
}

static ngx_int_t
ngx_http_vod_hls_handle_master_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;

	if (submodule_context->conf->hls.absolute_master_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &empty_string, &base_url);
	}

	rc = m3u8_builder_build_master_playlist(
		&submodule_context->request_context,
		&submodule_context->conf->hls.m3u8_config,
		&base_url,
		submodule_context->request_params.uses_multi_uri,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_master_playlist: m3u8_builder_build_master_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = m3u8_content_type;
	content_type->len = sizeof(m3u8_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t 
ngx_http_vod_hls_handle_index_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	hls_encryption_params_t encryption_params;
	ngx_str_t segments_base_url = ngx_null_string;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	u_char iv[AES_BLOCK_SIZE];

	if (submodule_context->conf->hls.absolute_index_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &submodule_context->r->uri, &base_url);

		ngx_http_vod_get_base_url(
			submodule_context->r, 
			&submodule_context->conf->https_header_name, 
			&submodule_context->conf->segments_base_url, 
			submodule_context->conf->segments_base_url_has_scheme, 
			&submodule_context->r->uri, 
			&segments_base_url);
	}

	ngx_http_vod_hls_init_encryption_params(&encryption_params, submodule_context, iv);

	rc = m3u8_builder_build_index_playlist(
		&submodule_context->request_context,
		&submodule_context->conf->hls.m3u8_config,
		&base_url,
		&segments_base_url,
		submodule_context->request_params.uses_multi_uri,
		&encryption_params,
		&submodule_context->conf->segmenter,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_index_playlist: m3u8_builder_build_index_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = m3u8_content_type;
	content_type->len = sizeof(m3u8_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_handle_iframe_playlist(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	mpeg_stream_metadata_t* cur_stream;
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	
	if (submodule_context->conf->hls.encryption_method != HLS_ENC_NONE)
	{
		ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: iframes playlist not supported with encryption");
		return NGX_HTTP_BAD_REQUEST;
	}

	for (cur_stream = submodule_context->mpeg_metadata.first_stream;
		cur_stream < submodule_context->mpeg_metadata.last_stream;
		cur_stream++)
	{
		if (cur_stream->media_info.media_type == MEDIA_TYPE_AUDIO &&
			cur_stream->media_info.speed_nom != cur_stream->media_info.speed_denom)
		{
			ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
				"ngx_http_vod_hls_handle_iframe_playlist: iframes playlist not supported with audio speed change");
			return NGX_HTTP_BAD_REQUEST;
		}
	}

	if (submodule_context->conf->hls.absolute_iframe_urls)
	{
		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &submodule_context->r->uri, &base_url);
	}

	rc = m3u8_builder_build_iframe_playlist(
		&submodule_context->request_context,
		&submodule_context->conf->hls.m3u8_config,
		&submodule_context->conf->hls.muxer_config,
		&base_url,
		submodule_context->request_params.uses_multi_uri,
		&submodule_context->conf->segmenter,
		&submodule_context->mpeg_metadata,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_iframe_playlist: m3u8_builder_build_iframe_playlist failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = m3u8_content_type;
	content_type->len = sizeof(m3u8_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_handle_encryption_key(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	u_char* encryption_key;

	encryption_key = ngx_palloc(submodule_context->request_context.pool, BUFFER_CACHE_KEY_SIZE);
	if (encryption_key == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_handle_encryption_key: ngx_palloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ngx_memcpy(encryption_key, submodule_context->request_params.suburis->encryption_key, BUFFER_CACHE_KEY_SIZE);

	response->data = encryption_key;
	response->len = BUFFER_CACHE_KEY_SIZE;

	content_type->data = encryption_key_content_type;
	content_type->len = sizeof(encryption_key_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_hls_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	read_cache_state_t* read_cache_state,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	hls_encryption_params_t encryption_params;
	hls_muxer_state_t* state;
	vod_status_t rc;
	bool_t simulation_supported;
	u_char iv[AES_BLOCK_SIZE];

	state = ngx_pcalloc(submodule_context->request_context.pool, sizeof(hls_muxer_state_t));
	if (state == NULL)
	{
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_frame_processor: ngx_pcalloc failed");
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ngx_http_vod_hls_init_encryption_params(&encryption_params, submodule_context, iv);

	rc = hls_muxer_init(
		state,
		&submodule_context->request_context,
		&submodule_context->conf->hls.muxer_config,
		&encryption_params,
		submodule_context->request_params.segment_index,
		&submodule_context->mpeg_metadata,
		read_cache_state,
		segment_writer->write_tail,
		segment_writer->context,
		&simulation_supported);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_hls_init_frame_processor: hls_muxer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (simulation_supported)
	{
		*response_size = hls_muxer_simulate_get_segment_size(state);
		hls_muxer_simulation_reset(state);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)hls_muxer_process;
	*frame_processor_state = state;

	content_type->len = sizeof(mpeg_ts_content_type) - 1;
	content_type->data = (u_char *)mpeg_ts_content_type;

	return NGX_OK;
}

static const ngx_http_vod_request_t hls_master_request = {
	0,
	PARSE_FLAG_TOTAL_SIZE_ESTIMATE | PARSE_FLAG_CODEC_NAME,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_hls_handle_master_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_index_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_BASIC_METADATA_ONLY,
	NULL,
	0,
	REQUEST_CLASS_MANIFEST,
	ngx_http_vod_hls_handle_index_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_iframes_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL_EXCEPT_OFFSETS | PARSE_FLAG_PARSED_EXTRA_DATA_SIZE,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_hls_handle_iframe_playlist,
	NULL,
};

static const ngx_http_vod_request_t hls_enc_key_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_BASIC_METADATA_ONLY,
	NULL,
	0,
	REQUEST_CLASS_OTHER,
	ngx_http_vod_hls_handle_encryption_key,
	NULL,
};

static const ngx_http_vod_request_t hls_segment_request = {
	REQUEST_FLAG_SINGLE_STREAM_PER_MEDIA_TYPE,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	NULL,
	0,
	REQUEST_CLASS_SEGMENT,
	NULL,
	ngx_http_vod_hls_init_frame_processor,
};

void
ngx_http_vod_hls_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_hls_loc_conf_t *conf)
{
	conf->absolute_master_urls = NGX_CONF_UNSET;
	conf->absolute_index_urls = NGX_CONF_UNSET;
	conf->absolute_iframe_urls = NGX_CONF_UNSET;
	conf->muxer_config.interleave_frames = NGX_CONF_UNSET;
	conf->muxer_config.align_frames = NGX_CONF_UNSET;
	conf->encryption_method = NGX_CONF_UNSET_UINT;
}

static char *
ngx_http_vod_hls_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_hls_loc_conf_t *conf,
	ngx_http_vod_hls_loc_conf_t *prev)
{
	ngx_conf_merge_value(conf->absolute_master_urls, prev->absolute_master_urls, 1);
	ngx_conf_merge_value(conf->absolute_index_urls, prev->absolute_index_urls, 1);
	ngx_conf_merge_value(conf->absolute_iframe_urls, prev->absolute_iframe_urls, 0);

	ngx_conf_merge_str_value(conf->master_file_name_prefix, prev->master_file_name_prefix, "master");
	ngx_conf_merge_str_value(conf->m3u8_config.index_file_name_prefix, prev->m3u8_config.index_file_name_prefix, "index");	
	ngx_conf_merge_str_value(conf->iframes_file_name_prefix, prev->iframes_file_name_prefix, "iframes");
	ngx_conf_merge_str_value(conf->m3u8_config.segment_file_name_prefix, prev->m3u8_config.segment_file_name_prefix, "seg");
	ngx_conf_merge_str_value(conf->m3u8_config.encryption_key_file_name, prev->m3u8_config.encryption_key_file_name, "encryption");

	ngx_conf_merge_value(conf->muxer_config.interleave_frames, prev->muxer_config.interleave_frames, 0);
	ngx_conf_merge_value(conf->muxer_config.align_frames, prev->muxer_config.align_frames, 1);

	ngx_conf_merge_uint_value(conf->encryption_method, prev->encryption_method, HLS_ENC_NONE);

	m3u8_builder_init_config(
		&conf->m3u8_config,
		base->segmenter.max_segment_duration, 
		conf->encryption_method);

	if (conf->encryption_method != HLS_ENC_NONE &&
		base->secret_key == NULL)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
			"\"vod_secret_key\" must be set when \"vod_hls_encryption_method\" is not none");
		return NGX_CONF_ERROR;
	}

	return NGX_CONF_OK;
}

static int 
ngx_http_vod_hls_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_hls_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	ngx_http_vod_request_params_t* request_params)
{
	bool_t expect_segment_index;
	ngx_int_t rc;

	// segment
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.segment_file_name_prefix, ts_file_ext))
	{
		start_pos += conf->hls.m3u8_config.segment_file_name_prefix.len;
		end_pos -= (sizeof(ts_file_ext) - 1);
		request_params->request = &hls_segment_request;
		expect_segment_index = TRUE;
	}
	// manifest
	else if (ngx_http_vod_ends_with_static(start_pos, end_pos, m3u8_file_ext))
	{
		end_pos -= (sizeof(m3u8_file_ext) - 1);

		// make sure the file name begins with 'index' or 'iframes'
		if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.m3u8_config.index_file_name_prefix))
		{
			request_params->request = &hls_index_request;
			start_pos += conf->hls.m3u8_config.index_file_name_prefix.len;
		}
		else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.iframes_file_name_prefix))
		{
			request_params->request = &hls_iframes_request;
			start_pos += conf->hls.iframes_file_name_prefix.len;
		}
		else if (ngx_http_vod_starts_with(start_pos, end_pos, &conf->hls.master_file_name_prefix))
		{
			request_params->request = &hls_master_request;
			start_pos += conf->hls.master_file_name_prefix.len;
		}
		else
		{
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
				"ngx_http_vod_hls_parse_uri_file_name: unidentified m3u8 request");
			return NGX_HTTP_BAD_REQUEST;
		}

		expect_segment_index = FALSE;
	}
	// encryption key
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->hls.m3u8_config.encryption_key_file_name, key_file_ext))
	{
		start_pos += conf->hls.m3u8_config.encryption_key_file_name.len;
		end_pos -= (sizeof(key_file_ext) - 1);
		request_params->request = &hls_enc_key_request;
		expect_segment_index = FALSE;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_hls_parse_uri_file_name: unidentified request");
		return NGX_HTTP_BAD_REQUEST;
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, expect_segment_index, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_hls_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

ngx_int_t
ngx_http_vod_hls_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
		"ngx_http_vod_hls_parse_drm_info: drm support for hls not implemented");
	return VOD_UNEXPECTED;
}

DEFINE_SUBMODULE(hls);
