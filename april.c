#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <stdbool.h>

#include "april.h"
#include "fbank.h"
#include "onnxruntime_c_api.h"
#include "ort_util.h"

const OrtApi* g_ort = NULL;

void aam_api_init(void){
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        fprintf(stderr, "Failed to init ONNX Runtime engine.\n");
        exit(-1);
    }
}

typedef struct ModelParameters {
    int batch_size;
    int segment_size;
    int segment_step;
    int mel_features;
    int sample_rate;
    int blank_id;

    int token_count;
    size_t token_length;
    
    char *tokens;
} ModelParameters;

char *get_token(ModelParameters *params, size_t token_index){
    return &params->tokens[params->token_length * token_index];
}

void read_params(ModelParameters *params, const char *params_file) {
    char buffer[64];
    int32_t *i32_ptr = (int32_t*)buffer;
    int64_t *i64_ptr = (int64_t*)buffer;

    FILE *fd = fopen(params_file, "r");
    fread(buffer, sizeof(int64_t), 1, fd);

    // 'PARAMS\0\0'
    const int64_t expected = 0x0000534D41524150L;
    assert(i64_ptr[0] == expected);

    fread(buffer, sizeof(int32_t), 7, fd);

    params->batch_size   = i32_ptr[0];
    params->segment_size = i32_ptr[1];
    params->segment_step = i32_ptr[2];
    params->mel_features = i32_ptr[3];
    params->sample_rate  = i32_ptr[4];
    params->token_count  = i32_ptr[5];
    params->blank_id     = i32_ptr[6];
    

    // Read all piece lengths and figure out the maximum
    size_t tokens_start = ftell(fd);
    
    params->token_length = 0;
    for(int i=0; i<params->token_count; i++){
        fread(buffer, sizeof(int32_t), 1, fd);
        int token_len = i32_ptr[0];
        if(token_len > params->token_length)
            params->token_length = token_len;
        
        fseek(fd, token_len, SEEK_CUR);
    }
    params->token_length += 1; // for '\0' byte

    // Allocate the memory
    printf("Tokens: %d, max size: %d\n", params->token_count, params->token_length);
    params->tokens = (char *)calloc(params->token_count, params->token_length);
    memset(params->tokens, '\0', params->token_count * params->token_length);

    // Rewind back and read
    fseek(fd, tokens_start, SEEK_SET);
    for(int i=0; i<params->token_count; i++){
        fread(buffer, sizeof(int32_t), 1, fd);
        int token_len = i32_ptr[0];
        
        assert(token_len < params->token_length);
        fread(get_token(params, i), 1, token_len, fd);
    }
}

void free_params(ModelParameters *params){
    free(params->tokens);
}



struct AprilASRModel_i {
    OrtEnv *env;
    OrtSessionOptions* session_options;

    OrtSession* encoder;
    OrtSession* joiner;

    // The comment numbers are for reference only, it may differ
    // with different sized models.
    int64_t x_dim[3];       // (1, 9, 80)
    int64_t h_dim[3];       // (12, 1, 512)
    int64_t c_dim[3];       // (12, 1, 1024)
    int64_t eout_dim[3];    // (1, 1, 512)
    int64_t context_dim[2]; // (1, 2)
    int64_t logits_dim[3];  // (1, 1, 500)

    FBankOptions fbank_opts;
    ModelParameters params;
};

AprilASRModel aam_create_model(const char *model_dir) {
    AprilASRModel aam = (AprilASRModel)calloc(1, sizeof(struct AprilASRModel_i));
    
    ORT_ABORT_ON_ERROR(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "test", &aam->env));
    assert(aam->env != NULL);

    ORT_ABORT_ON_ERROR(g_ort->CreateSessionOptions(&aam->session_options));
    ORT_ABORT_ON_ERROR(g_ort->SetIntraOpNumThreads(aam->session_options, 1));
    ORT_ABORT_ON_ERROR(g_ort->SetInterOpNumThreads(aam->session_options, 1));

    // TODO later later: maybe combine everything into one nice big file
    ORTCHAR_T model_path[1024];
    
    SET_CONCAT_PATH(model_path, model_dir, "encoder.onnx");
    ORT_ABORT_ON_ERROR(g_ort->CreateSession(aam->env, model_path, aam->session_options, &aam->encoder));
    
    SET_CONCAT_PATH(model_path, model_dir, "joiner.onnx");
    ORT_ABORT_ON_ERROR(g_ort->CreateSession(aam->env, model_path, aam->session_options, &aam->joiner));

    SET_CONCAT_PATH(model_path, model_dir, "params.bin");
    read_params(&aam->params, model_path);

    assert(input_count(aam->encoder)  == 3);
    assert(output_count(aam->encoder) == 3);
    
    assert(input_count(aam->joiner)  == 2);
    assert(output_count(aam->joiner) == 1);

    assert(input_dims(aam->encoder, 0, aam->x_dim, 3) == 3);
    assert(input_dims(aam->encoder, 1, aam->h_dim, 3) == 3);
    assert(input_dims(aam->encoder, 2, aam->c_dim, 3) == 3);
    assert(output_dims(aam->encoder, 0, aam->eout_dim, 3) == 3);

    assert(input_dims(aam->joiner, 0, aam->context_dim, 2) == 2);
    assert(output_dims(aam->joiner, 0, aam->logits_dim, 3) == 3);

    aam->fbank_opts.sample_freq = aam->params.sample_rate;
    aam->fbank_opts.num_bins    = aam->params.mel_features;
    aam->fbank_opts.pull_segment_count = aam->params.segment_size;
    aam->fbank_opts.pull_segment_step  = aam->params.segment_step;

    // TODO: read these from config file
    aam->fbank_opts.frame_shift_ms = 10;
    aam->fbank_opts.frame_length_ms = 25;
    aam->fbank_opts.round_pow2 = true;
    aam->fbank_opts.mel_low = 20;
    aam->fbank_opts.mel_high = 0;
    aam->fbank_opts.snip_edges = true;

    assert(aam->x_dim[0] == aam->params.batch_size);
    assert(aam->x_dim[1] == aam->fbank_opts.pull_segment_count);
    assert(aam->x_dim[2] == aam->fbank_opts.num_bins);
    assert(aam->logits_dim[2] == aam->params.token_count);

    return aam;
}


void aam_free(AprilASRModel model) {
    free_params(&model->params);

    g_ort->ReleaseSession(model->joiner);
    g_ort->ReleaseSession(model->encoder);
    g_ort->ReleaseSessionOptions(model->session_options);
    g_ort->ReleaseEnv(model->env);

    free(model);
}

struct AprilASRSession_i {
    AprilASRModel model;
    OnlineFBank fbank;

    OrtMemoryInfo *memory_info;

    TensorF x;

    bool hc_use_0;
    TensorF h[2];
    TensorF c[2];

    TensorF eout;

    TensorI context;
    TensorF logits;

    int64_t active_tokens[64];
    size_t active_token_head;
};


AprilASRSession aas_create_session(AprilASRModel model) {
    AprilASRSession aas = (AprilASRSession)calloc(1, sizeof(struct AprilASRSession_i));

    aas->model = model;
    aas->fbank = make_fbank(model->fbank_opts);

    ORT_ABORT_ON_ERROR(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &aas->memory_info));
    OrtMemoryInfo *mi = aas->memory_info;

    aas->x = alloc_tensor3f(mi, model->x_dim);
    for(int i=0; i<2; i++){
        aas->h[i] = alloc_tensor3f(mi, model->h_dim);
        aas->c[i] = alloc_tensor3f(mi, model->c_dim);
    }

    aas->eout = alloc_tensor3f(mi, model->eout_dim);

    aas->context = alloc_tensor2i(mi, model->context_dim);
    for(int i=0; i<SHAPE_PRODUCT2(model->context_dim); i++) aas->context.data[i] = 0;

    aas->logits = alloc_tensor3f(mi, model->logits_dim);

    aas->hc_use_0 = false;
    aas->active_token_head = 0;

    assert(aas->fbank          != NULL);
    assert(aas->x.tensor       != NULL);
    assert(aas->h[0].tensor    != NULL);
    assert(aas->c[0].tensor    != NULL);
    assert(aas->h[1].tensor    != NULL);
    assert(aas->c[1].tensor    != NULL);
    assert(aas->eout.tensor    != NULL);
    assert(aas->context.tensor != NULL);
    assert(aas->logits.tensor  != NULL);

    return aas;
}

void aas_free(AprilASRSession session) {
    free_tensorf(&session->logits);
    free_tensori(&session->context);
    free_tensorf(&session->eout);
    for(int i=0; i<2; i++) {
        free_tensorf(&session->c[i]);
        free_tensorf(&session->h[i]);
    }

    free_tensorf(&session->x);
    g_ort->ReleaseMemoryInfo(session->memory_info);
    free_fbank(session->fbank);

    free(session);
}

const char* encoder_input_names[] = {"x", "h", "c"};
const char* encoder_output_names[] = {"encoder_out", "next_h", "next_c"};

const char* joiner_input_names[] = {"context", "encoder_out"};
const char* joiner_output_names[] = {"logits"};

// Runs encoder on current data in aas->x
void aas_run_encoder(AprilASRSession aas){
    aas->hc_use_0 = !aas->hc_use_0;
    const OrtValue *inputs[] = {
        aas->x.tensor,
        aas->h[aas->hc_use_0 ? 0 : 1].tensor,
        aas->c[aas->hc_use_0 ? 0 : 1].tensor
    };

    OrtValue *outputs[] = {
        aas->eout.tensor,
        aas->h[aas->hc_use_0 ? 1 : 0].tensor,
        aas->c[aas->hc_use_0 ? 1 : 0].tensor
    };

    ORT_ABORT_ON_ERROR(g_ort->Run(aas->model->encoder, NULL,
                                    encoder_input_names, inputs, 3,
                                    encoder_output_names, 3, outputs));
}

// Runs joiner on current data in aas->context and aas->eout
void aas_run_joiner(AprilASRSession aas){
    const OrtValue *inputs[] = {
        aas->context.tensor,
        aas->eout.tensor
    };
    
    OrtValue *outputs[] = {
        aas->logits.tensor
    };

    ORT_ABORT_ON_ERROR(g_ort->Run(aas->model->joiner, NULL,
                                    joiner_input_names, inputs, 2,
                                    joiner_output_names, 1, outputs));
}

// Processes current data in aas->logits. Returns true if new token was
// added, else returns false if no new data is available. Updates
// aas->context and aas->active_tokens. Uses basic greedy search algorithm.
bool aas_process_logits(AprilASRSession aas, float early_emit){
    ModelParameters *params = &aas->model->params;
    float *logits = aas->logits.data;

    logits[0] -= early_emit;

    int max_idx = -1;
    int max_idx_non0 = -1;
    float max_val = -9999999999.0;
    float max_val_non0 = -9999999999.0;
    for(int i=0; i<500; i++){
        if(logits[i] > max_val){
            max_idx = i;
            max_val = logits[i];
        }

        if((logits[i] > max_val_non0) && (i > 0)){
            max_idx_non0 = i;
            max_val_non0 = logits[i];
        }
    }



    fprintf(stderr, "\r");
    for(int i=0; i<80; i++){
        fprintf(stderr, " ");
    }
    fprintf(stderr, "\r");
    for(int m=0; m<aas->active_token_head; m++){
        fprintf(stderr, "%s", get_token(params, aas->active_tokens[m]));
    }

    //char p = '\r';
    if(max_idx != 0) {
        if(aas->active_token_head > 16){
            if((get_token(params, max_idx)[0] == ' ') || (aas->active_token_head > 30)) {
                aas->active_token_head = 0;
                //p = '\n';
                fprintf(stderr, "\n");
            }
        }

        aas->active_tokens[aas->active_token_head] = max_idx;
        aas->active_token_head++;

        aas->context.data[0] = aas->context.data[1];
        aas->context.data[1] = (int64_t)max_idx;

        fprintf(stderr, "%s", get_token(params, max_idx));

        return true;
    }else if((max_idx == 0)
        && (aas->context.data[1] != max_idx_non0)
        && (max_val_non0 > (max_val - 6.0f))
        && ((aas->active_token_head <= 16) || (get_token(params, max_idx_non0)[0] != ' '))
    ) {
        fprintf(stderr, "%s", get_token(params, max_idx_non0));
        return false;
    }
}

#define SEGSIZE 3200 //TODO
void aas_feed_pcm16(AprilASRSession aas, short *pcm16, size_t short_count) {
    assert(aas->fbank != NULL);
    assert(aas != NULL);
    assert(pcm16 != NULL);

    size_t head = 0;
    float wave[SEGSIZE];

    while(head < short_count){
        size_t remaining = short_count - head;
        if(remaining < 1) break;
        if(remaining > SEGSIZE) remaining = SEGSIZE;

        for(int i=0; i<remaining; i++){
            wave[i] = (float)pcm16[head + i] / 32768.0f;
        }
        
        fbank_accept_waveform(aas->fbank, wave, remaining);

        while(fbank_pull_segments( aas->fbank, aas->x.data, sizeof(float)*SHAPE_PRODUCT3(aas->model->x_dim) )){
            aas_run_encoder(aas);

            float early_emit = 3.0f;
            for(int i=0; i<8; i++){
                early_emit -= 1.0f;
                aas_run_joiner(aas);
                if(!aas_process_logits(aas, early_emit > 0.0f ? early_emit : 0.0f)) break;
            }
        }

        head += remaining;
    }
}
