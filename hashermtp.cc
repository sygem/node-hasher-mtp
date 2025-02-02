#include <node.h>
#include <node_buffer.h>
#include <v8.h>
#include <stdint.h>
#include <iostream>
#include "nan.h"
#include "merkletree/mtp.h"
#include "compat.h"
#include "uint256.h"

using namespace node;
using namespace v8;

#if ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3))
#define WANT_BUILTIN_BSWAP
#else
#define bswap_32(x) ((((x) << 24) & 0xff000000u) | (((x) << 8) & 0x00ff0000u) \
                   | (((x) >> 8) & 0x0000ff00u) | (((x) >> 24) & 0x000000ffu))
#endif

static inline uint32_t swab32(uint32_t v)
{
#ifdef WANT_BUILTIN_BSWAP
    return __builtin_bswap32(v);
#else
    return bswap_32(v);
#endif
}

#if !HAVE_DECL_BE32DEC
static inline uint32_t be32dec(const void *pp)
{
    const uint8_t *p = (uint8_t const *)pp;
    return ((uint32_t)(p[3]) + ((uint32_t)(p[2]) << 8) +
            ((uint32_t)(p[1]) << 16) + ((uint32_t)(p[0]) << 24));
}
#endif

#if !HAVE_DECL_LE32DEC
static inline uint32_t le32dec(const void *pp)
{
    const uint8_t *p = (uint8_t const *)pp;
    return ((uint32_t)(p[0]) + ((uint32_t)(p[1]) << 8) +
            ((uint32_t)(p[2]) << 16) + ((uint32_t)(p[3]) << 24));
}
#endif

#if !HAVE_DECL_BE32ENC
static inline void be32enc(void *pp, uint32_t x)
{
    uint8_t *p = (uint8_t *)pp;
    p[3] = x & 0xff;
    p[2] = (x >> 8) & 0xff;
    p[1] = (x >> 16) & 0xff;
    p[0] = (x >> 24) & 0xff;
}
#endif

#if !HAVE_DECL_LE32ENC
static inline void le32enc(void *pp, uint32_t x)
{
    uint8_t *p = (uint8_t *)pp;
    p[0] = x & 0xff;
    p[1] = (x >> 8) & 0xff;
    p[2] = (x >> 16) & 0xff;
    p[3] = (x >> 24) & 0xff;
}
#endif

#if !HAVE_DECL_LE16DEC
static inline uint16_t le16dec(const void *pp)
{
    const uint8_t *p = (uint8_t const *)pp;
    return ((uint16_t)(p[0]) + ((uint16_t)(p[1]) << 8));
}
#endif

#if !HAVE_DECL_LE16ENC
static inline void le16enc(void *pp, uint16_t x)
{
    uint8_t *p = (uint8_t *)pp;
    p[0] = x & 0xff;
    p[1] = (x >> 8) & 0xff;
}
#endif

#define THROW_ERROR_EXCEPTION(x) Nan::ThrowError(x)

namespace {

    const int8_t L = 64;
    const int8_t L_TCR = 16;
    const int VEC_SIZE = 16;
    const int NONCE_SIZE = sizeof(uint32_t);
    const int HASH_ROOT_SIZE = sizeof(uint8_t) * 16;
    const int BLOCK_SIZE = sizeof(uint64_t) * L * 2 * 128;
    const int BLOCK_SIZE_TCR = sizeof(uint64_t) * L_TCR * 2 * 128;
    const int HASH_VALUE_SIZE = 32;
}


NAN_METHOD(hash) {

        if (info.Length() < 5) {
            return THROW_ERROR_EXCEPTION("hasher-mtp.hash - 5 arguments expected.");
        }

        uint32_t* header_ptr = (uint32_t *)Buffer::Data(info[0]->ToObject());
        char* target_ptr = (char*)Buffer::Data(info[1]->ToObject());
        char* nonce_start_ptr = (char*)Buffer::Data(info[2]->ToObject());
        char* nonce_end_ptr = (char*)Buffer::Data(info[3]->ToObject());
        char* output_ptr = (char*)Buffer::Data(info[4]->ToObject());

        uint256 target;
        uint256 hash_value;

        // target
        std::memcpy(target.begin(), target_ptr, target.size());

        // proofs
        uint8_t hash_root[16];
        uint64_t block[L*2][128];
        std::deque<std::vector<uint8_t>> proof[L * 3];

        uint32_t nonce_start = *(uint32_t*)nonce_start_ptr;
        uint32_t nonce_end = *(uint32_t*)nonce_end_ptr;

        uint64_t hashes_done = 0;

        struct timeval tv_start, tv_end, timediff;
        unsigned char merkle_root[16];
        unsigned char hash[32];

        mtp::MerkleTree::Elements ordered_tree_elements;

        argon2_context context = mtp::init_argon2d_param((const char*)header_ptr); //endiandata
        argon2_instance_t instance;
        argon2_ctx_from_mtp(&context, &instance);
        ordered_tree_elements = mtp::init(&instance);
        mtp::MerkleTree ordered_tree(ordered_tree_elements, true);
        mtp::MerkleTree::Buffer root = ordered_tree.getRoot();
        std::copy(root.begin(), root.end(), merkle_root);

        uint32_t nonce = nonce_start;
        info.GetReturnValue().Set(Nan::False());

        do {

            if (nonce == UINT32_MAX || nonce >= nonce_end)
            {
                break;
            }

            bool is_sol = mtp::solver_fast(nonce, &instance, merkle_root, header_ptr, target);

            if (is_sol) {
                uint64_t mtp_block[L * 2][128];
                unsigned char mtp_proof[L * 3 * 353];
                unsigned int mtp_proof_size = 0;

                mtp::solver(
                        /* input              */ header_ptr,
                        /* nonce              */ nonce,
                        /* instance           */ &instance,
                        /* merkle_tree        */ ordered_tree,
                        /* merkle_root        */ merkle_root,
                        /* mtp_block_out      */ mtp_block,
                        /* mtp_proof_out      */ mtp_proof,
                        /* mtp_proof_size_out */ &mtp_proof_size,
                        /* hash_out           */ hash);

                int res = 1;

                hashes_done = nonce - nonce_start;

                info.GetReturnValue().Set(Nan::True());

                char* output_pos = output_ptr;

                // output data size
                uint32_t result_size =
                        NONCE_SIZE +
                        HASH_VALUE_SIZE +
                        HASH_ROOT_SIZE +
                        BLOCK_SIZE +
                        mtp_proof_size;

                // result_size
                std::memcpy(output_pos, &result_size, sizeof(uint32_t));
                output_pos += sizeof(uint32_t);

                // nonce
                std::memcpy(output_pos, &nonce, NONCE_SIZE);
                output_pos += NONCE_SIZE;

                // hash_value
                std::memcpy(output_pos, hash, HASH_VALUE_SIZE);
                output_pos += HASH_VALUE_SIZE;

                // hash_root_mtp
                std::memcpy(output_pos, merkle_root, HASH_ROOT_SIZE);
                output_pos += HASH_ROOT_SIZE;

                // block_mtp
                std::memcpy(output_pos, mtp_block, BLOCK_SIZE);
                output_pos += BLOCK_SIZE;

                std::memcpy(output_pos, mtp_proof, mtp_proof_size);

                break;
            }

            nonce += 1;

        } while (nonce_start<0xffffffff);

        ordered_tree_elements.clear();
        free(instance.memory);
}


NAN_METHOD(hash_one) {

        if (info.Length() < 3)
            return THROW_ERROR_EXCEPTION("hasher-mtp.hash_one - 3 arguments expected.");

        uint32_t* header_ptr = (uint32_t *)Buffer::Data(info[0]->ToObject());
        char* nonce_ptr = (char*)Buffer::Data(info[1]->ToObject());
        char* output_ptr = (char*)Buffer::Data(info[2]->ToObject());

        uint256 hash_value;

        // proofs
        uint8_t hash_root[16];
        uint64_t block[L*2][128];
        std::deque<std::vector<uint8_t>> proof[L * 3];

        uint32_t nonce = *(uint32_t*)nonce_ptr;

        uint64_t hashes_done = 0;

        struct timeval tv_start, tv_end, timediff;
        unsigned char merkle_root[16];
        unsigned char hash[32];

        mtp::MerkleTree::Elements ordered_tree_elements;

        argon2_context context = mtp::init_argon2d_param((const char*)header_ptr); //endiandata
        argon2_instance_t instance;
        argon2_ctx_from_mtp(&context, &instance);
        ordered_tree_elements = mtp::init(&instance);
        mtp::MerkleTree ordered_tree(ordered_tree_elements, true);
        mtp::MerkleTree::Buffer root = ordered_tree.getRoot();
        std::copy(root.begin(), root.end(), merkle_root);

        uint64_t mtp_block[L * 2][128];
        unsigned char mtp_proof[L * 3 * 353];
        unsigned int mtp_proof_size = 0;

        bool is_valid = mtp::solver(
        /* input              */ header_ptr,
        /* nonce              */ nonce,
        /* instance           */ &instance,
        /* merkle_tree        */ ordered_tree,
        /* merkle_root        */ merkle_root,
        /* mtp_block_out      */ mtp_block,
        /* mtp_proof_out      */ mtp_proof,
        /* mtp_proof_size_out */ &mtp_proof_size,
        /* hash_out           */ hash);

        if (is_valid) {
            info.GetReturnValue().Set(Nan::True());
        }
        else {
            info.GetReturnValue().Set(Nan::False());
            return;
        }

        char *output_pos = output_ptr;

        // output data size
        uint32_t result_size =
        NONCE_SIZE +
        HASH_VALUE_SIZE +
        HASH_ROOT_SIZE +
        BLOCK_SIZE +
        mtp_proof_size;

        // result_size
        std::memcpy(output_pos, &result_size, sizeof(uint32_t));
        output_pos += sizeof(uint32_t);

        // nonce
        std::memcpy(output_pos, &nonce, NONCE_SIZE);
        output_pos += NONCE_SIZE;

        // hash_value
        std::memcpy(output_pos, hash, HASH_VALUE_SIZE);
        output_pos += HASH_VALUE_SIZE;

        // hash_root_mtp
        std::memcpy(output_pos, merkle_root, HASH_ROOT_SIZE);
        output_pos += HASH_ROOT_SIZE;

        // block_mtp
        std::memcpy(output_pos, mtp_block, BLOCK_SIZE);
        output_pos += BLOCK_SIZE;

        std::memcpy(output_pos, mtp_proof, mtp_proof_size);


        ordered_tree_elements.clear();
        free(instance.memory);
}


NAN_METHOD(verify) {

        if (info.Length() < 6)
            return THROW_ERROR_EXCEPTION("hasher-mtp.verify - 6 arguments expected.");

        char* input_ptr = (char*)Buffer::Data(info[0]->ToObject());
        unsigned char* nonce_ptr = (unsigned char*)Buffer::Data(info[1]->ToObject());
        unsigned char* hash_root_ptr = (unsigned char*)Buffer::Data(info[2]->ToObject());
        unsigned char* block_ptr = (unsigned char*)Buffer::Data(info[3]->ToObject());
        unsigned char* proof_ptr = (unsigned char*)Buffer::Data(info[4]->ToObject());
        unsigned char* hash_value_out_ptr = (unsigned char*)Buffer::Data(info[5]->ToObject());

        // hash root
        uint8_t hash_root[16];
        std::memcpy(&hash_root, hash_root_ptr, sizeof(hash_root));

        // block
        uint64_t block[L*2][128];
        std::memcpy(&block, block_ptr, sizeof(block));

        uint32_t nonce = *(uint32_t*)nonce_ptr;
        uint256 hash_value;

        bool is_valid = mtp::verify_fast(input_ptr, nonce, hash_root, block, &hash_value);

        if (!is_valid) {
            info.GetReturnValue().Set(Nan::False());
        }
        else {

            // proof
            std::deque <std::vector<uint8_t>> proof[L * 3];
            unsigned char *proof_pos = proof_ptr;

            for (int i = 0; i < L * 3; ++i) {

                uint8_t deq_size;
                std::memcpy(&deq_size, proof_pos, sizeof(uint8_t));
                proof_pos += sizeof(uint8_t);

                std::deque <std::vector<uint8_t>> deq(deq_size);

                for (int j = 0; j < deq_size; ++j) {
                    std::vector <uint8_t> vec(sizeof(uint8_t) * VEC_SIZE);
                    std::memcpy(vec.data(), proof_pos, sizeof(uint8_t) * VEC_SIZE);
                    proof_pos += sizeof(uint8_t) * VEC_SIZE;
                    deq[j] = vec;
                }
                proof[i] = deq;
            }

            is_valid = mtp::verify(input_ptr, nonce, hash_root, block, proof, &hash_value);

            if (is_valid) {
                std::memcpy(hash_value_out_ptr, hash_value.begin(), hash_value.size());
                info.GetReturnValue().Set(Nan::True());
            } else {
                info.GetReturnValue().Set(Nan::False());
            }
        }
}


NAN_METHOD(verify_tcr) {

        if (info.Length() < 6)
            return THROW_ERROR_EXCEPTION("hasher-mtp.verify_tcr - 6 arguments expected.");

        char* input_ptr = (char*)Buffer::Data(info[0]->ToObject());
        unsigned char* nonce_ptr = (unsigned char*)Buffer::Data(info[1]->ToObject());
        unsigned char* hash_root_ptr = (unsigned char*)Buffer::Data(info[2]->ToObject());
        unsigned char* block_ptr = (unsigned char*)Buffer::Data(info[3]->ToObject());
        unsigned char* proof_ptr = (unsigned char*)Buffer::Data(info[4]->ToObject());
        unsigned char* hash_value_out_ptr = (unsigned char*)Buffer::Data(info[5]->ToObject());

        // hash root
        uint8_t hash_root[16];
        std::memcpy(&hash_root, hash_root_ptr, sizeof(hash_root));

        // block
        uint64_t block[L_TCR*2][128];
        std::memcpy(&block, block_ptr, sizeof(block));

        uint32_t nonce = *(uint32_t*)nonce_ptr;
        uint256 hash_value;

        bool is_valid = mtp::verify_fast_tcr(input_ptr, nonce, hash_root, block, &hash_value);

        if (!is_valid) {
            info.GetReturnValue().Set(Nan::False());
        }
        else {

            // proof
            std::deque <std::vector<uint8_t>> proof[L_TCR * 3];
            unsigned char *proof_pos = proof_ptr;

            for (int i = 0; i < L_TCR * 3; ++i) {

                uint8_t deq_size;
                std::memcpy(&deq_size, proof_pos, sizeof(uint8_t));
                proof_pos += sizeof(uint8_t);

                std::deque <std::vector<uint8_t>> deq(deq_size);

                for (int j = 0; j < deq_size; ++j) {
                    std::vector <uint8_t> vec(sizeof(uint8_t) * VEC_SIZE);
                    std::memcpy(vec.data(), proof_pos, sizeof(uint8_t) * VEC_SIZE);
                    proof_pos += sizeof(uint8_t) * VEC_SIZE;
                    deq[j] = vec;
                }
                proof[i] = deq;
            }

            is_valid = mtp::verify_tcr(input_ptr, nonce, hash_root, block, proof, &hash_value);

            if (is_valid) {
                std::memcpy(hash_value_out_ptr, hash_value.begin(), hash_value.size());
                info.GetReturnValue().Set(Nan::True());
            } else {
                info.GetReturnValue().Set(Nan::False());
            }
        }
}


NAN_METHOD(verify_fast) {

        if (info.Length() < 5)
            return THROW_ERROR_EXCEPTION("hasher-mtp.verify_fast - 5 arguments expected.");

        char* input_ptr = (char*)Buffer::Data(info[0]->ToObject());
        unsigned char* nonce_ptr = (unsigned char*)Buffer::Data(info[1]->ToObject());
        unsigned char* hash_root_ptr = (unsigned char*)Buffer::Data(info[2]->ToObject());
        unsigned char* block_ptr = (unsigned char*)Buffer::Data(info[3]->ToObject());
        unsigned char* hash_value_out_ptr = (unsigned char*)Buffer::Data(info[4]->ToObject());

        // hash root
        uint8_t hash_root[16];
        std::memcpy(&hash_root, hash_root_ptr, sizeof(hash_root));

        // block
        uint64_t block[L*2][128];
        std::memcpy(&block, block_ptr, sizeof(block));

        uint32_t nonce = *(uint32_t*)nonce_ptr;
        uint256 hash_value;

        bool is_valid = mtp::verify_fast(input_ptr, nonce, hash_root, block, &hash_value);

        if (is_valid) {
            std::memcpy(hash_value_out_ptr, hash_value.begin(), hash_value.size());
            info.GetReturnValue().Set(Nan::True());
        }
        else {
            info.GetReturnValue().Set(Nan::False());
        }
}

NAN_METHOD(verify_fast_tcr) {

        if (info.Length() < 5)
            return THROW_ERROR_EXCEPTION("hasher-mtp.verify_fast_tcr - 5 arguments expected.");

        char* input_ptr = (char*)Buffer::Data(info[0]->ToObject());
        unsigned char* nonce_ptr = (unsigned char*)Buffer::Data(info[1]->ToObject());
        unsigned char* hash_root_ptr = (unsigned char*)Buffer::Data(info[2]->ToObject());
        unsigned char* block_ptr = (unsigned char*)Buffer::Data(info[3]->ToObject());
        unsigned char* hash_value_out_ptr = (unsigned char*)Buffer::Data(info[4]->ToObject());

        // hash root
        uint8_t hash_root[16];
        std::memcpy(&hash_root, hash_root_ptr, sizeof(hash_root));

        // block
        uint64_t block[L_TCR*2][128];
        std::memcpy(&block, block_ptr, sizeof(block));

        uint32_t nonce = *(uint32_t*)nonce_ptr;
        uint256 hash_value;

        bool is_valid = mtp::verify_fast_tcr(input_ptr, nonce, hash_root, block, &hash_value);

        if (is_valid) {
            std::memcpy(hash_value_out_ptr, hash_value.begin(), hash_value.size());
            info.GetReturnValue().Set(Nan::True());
        }
        else {
            info.GetReturnValue().Set(Nan::False());
        }
}


NAN_MODULE_INIT(init) {
        Nan::Set(target, Nan::New("hash").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(hash)).ToLocalChecked());
        Nan::Set(target, Nan::New("hash_one").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(hash_one)).ToLocalChecked());
        Nan::Set(target, Nan::New("verify").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(verify)).ToLocalChecked());
        Nan::Set(target, Nan::New("verify_tcr").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(verify_tcr)).ToLocalChecked());
        Nan::Set(target, Nan::New("verify_fast").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(verify_fast)).ToLocalChecked());
        Nan::Set(target, Nan::New("verify_fast_tcr").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(verify_fast_tcr)).ToLocalChecked());
}

NODE_MODULE(hashermtp, init)