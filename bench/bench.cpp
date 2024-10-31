#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>

#include "cmd_line_parser/parser.hpp"
#include "essentials/essentials.hpp"
#include "tinyformat/tinyformat.h"

static constexpr int SEARCH_RUNS = 10;
static constexpr uint64_t NOT_FOUND = UINT64_MAX;
static const char* TMP_INDEX_FILENAME = "tmp.bin";

std::vector<std::string> load_strings(const std::string& filepath, bool to_unique) {
    std::ifstream ifs(filepath);
    if (!ifs) {
        tfm::errorfln("Failed to open %s", filepath);
        exit(1);
    }
    std::vector<std::string> strings;
    for (std::string line; std::getline(ifs, line);) {
        strings.push_back(line);
    }
    if (to_unique) {
        std::sort(strings.begin(), strings.end());
        strings.erase(std::unique(strings.begin(), strings.end()), strings.end());
    }
    return strings;
}
std::vector<std::string> sample_strings(const std::vector<std::string>& strings, std::uint64_t num_samples,
                                        std::uint64_t random_seed) {
    std::mt19937_64 engine(random_seed);
    std::uniform_int_distribution<std::uint64_t> dist(0, strings.size() - 1);

    std::vector<std::string> sampled(num_samples);
    for (std::uint64_t i = 0; i < num_samples; i++) {
        sampled[i] = strings[dist(engine)];
    }
    return sampled;
}

typedef struct {
    bool force_asc;
} build_opts;

template <class T>
std::unique_ptr<T> build(std::vector<std::string>&, build_opts& opts);
template <class T>
uint64_t lookup(T*, const std::string&);
template <class T>
uint64_t decode(T*, uint64_t);
template <class T>
uint64_t get_memory(T*);

#ifdef USE_FST
#include <fst.hpp>
using trie_t = fst::Trie;
static const uint32_t SPARSE_DENSE_RATIO = 16;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    auto trie = std::make_unique<trie_t>(keys, true, SPARSE_DENSE_RATIO);
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    auto res = trie->exactSearch(query);
    return res != fst::kNotFound ? uint64_t(res) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return 0;
}
template <>
uint64_t get_memory(trie_t* trie) {
    std::ofstream ofs(TMP_INDEX_FILENAME);
    trie->save(ofs);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_DARTS
#include "darts/darts.h"
using trie_t = Darts::DoubleArray;
#endif
#ifdef USE_DARTSC
#include "darts-clone/darts.h"
using trie_t = Darts::DoubleArrayImpl<void, void, int32_t, void>;
#endif
#if defined(USE_DARTS) || defined(USE_DARTSC)
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& key_strs, build_opts& opts) {
    std::size_t num_keys = key_strs.size();
    std::vector<const char*> keys(num_keys);
    for (std::size_t i = 0; i < num_keys; ++i) {
        keys[i] = key_strs[i].c_str();
    }

    auto trie = std::make_unique<trie_t>();
    trie->build(num_keys, keys.data());
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    auto res = trie->exactMatchSearch<int32_t>(query.c_str(), query.length());
    return res != -1 ? uint64_t(res) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return 0;
}
template <>
uint64_t get_memory(trie_t* trie) {
    trie->save(TMP_INDEX_FILENAME);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_CEDAR
#include "cedar/cedar.h"
#endif
#ifdef USE_CEDARPP
#include "cedar/cedarpp.h"
#endif
#if defined(USE_CEDAR) || defined(USE_CEDARPP)
using trie_t = cedar::da<uint32_t>;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    auto trie = std::make_unique<trie_t>();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        trie->update(keys[i].c_str(), keys[i].size(), uint32_t(i));
    }
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    auto res = trie->exactMatchSearch<uint32_t>(query.c_str(), query.length());
    return res != uint32_t(trie_t::error_code::CEDAR_NO_VALUE) ? uint64_t(res) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return 0;
}
template <>
uint64_t get_memory(trie_t* trie) {
    trie->save(TMP_INDEX_FILENAME);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_DASTRIE
#include "dastrie/dastrie.h"
using trie_t = dastrie::trie<uint32_t>;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    using builder_t = dastrie::builder<const char*, uint32_t>;
    using record_t = builder_t::record_type;

    std::vector<record_t> records(keys.size());
    for (uint32_t i = 0; i < keys.size(); ++i) {
        records[i] = record_t{keys[i].c_str(), i};
    }

    builder_t builder;
    builder.build(records.data(), records.data() + records.size());

    {
        std::ofstream ofs(TMP_INDEX_FILENAME, std::ios::binary);
        builder.write(ofs);
    }

    std::ifstream ifs(TMP_INDEX_FILENAME, std::ios::binary);
    auto trie = std::make_unique<trie_t>();
    trie->read(ifs);
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    auto res = trie->get(query.c_str(), UINT32_MAX);
    return res != UINT32_MAX ? uint64_t(res) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return 0;
}
template <>
uint64_t get_memory(trie_t* trie) {
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_TX
#include <tx.hpp>
using trie_t = tx_tool::tx;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    {
        trie_t trie;
        trie.build(keys, TMP_INDEX_FILENAME);
    }
    auto trie = std::make_unique<trie_t>();
    trie->read(TMP_INDEX_FILENAME);
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    static size_t retLen = 0;
    auto res = trie->prefixSearch(query.c_str(), query.length(), retLen);
    return res != tx_tool::tx::NOTFOUND ? uint64_t(res) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    static std::string ret;
    trie->reverseLookup(tx_tool::uint(query), ret);
    return ret.size();
}
template <>
uint64_t get_memory(trie_t*) {
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_MARISA
#include <marisa.h>
using trie_t = marisa::Trie;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    marisa::Keyset keyset;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keyset.push_back(keys[i].c_str(), keys[i].length(), 1.0F);
    }
    marisa::NodeOrder param_node_order = MARISA_DEFAULT_ORDER;
    if (opts.force_asc)
        param_node_order = MARISA_LABEL_ORDER;
    auto trie = std::make_unique<trie_t>();
    trie->build(keyset, param_node_order);
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    static marisa::Agent agent;
    agent.set_query(query.c_str(), query.length());
    return trie->lookup(agent) ? uint64_t(agent.key().id()) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    static marisa::Agent agent;
    agent.set_query(query);
    trie->reverse_lookup(agent);
    return agent.key().length();
}
template <>
uint64_t get_memory(trie_t* trie) {
    trie->save(TMP_INDEX_FILENAME);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_MADRAS
#include <madras_builder_dv1.hpp>
#include <madras_dv1.hpp>
class cleanup_madras : public madras_dv1::cleanup_interface {
    private:
        std::vector<uint8_t> *output_buf;
    public:
        virtual ~cleanup_madras() {
        }
        void release() {
            delete output_buf;
        }
        void init(std::vector<uint8_t> *_output_buf) {
            output_buf = _output_buf;
        }
};
using trie_t = madras_dv1::static_trie_map;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    madras_dv1::bldr_options bldr_opts = madras_dv1::dflt_opts;
    if (opts.force_asc) {
        bldr_opts.dart = true;
        bldr_opts.sort_nodes_on_freq = false;
    }
    madras_dv1::builder trie_bldr(nullptr, "kv_table,Key", 1, "t", "u",
                0, true, false, bldr_opts);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        trie_bldr.insert((const uint8_t *) keys[i].c_str(), keys[i].length());
    }
    std::vector<uint8_t> *output_buf = new std::vector<uint8_t>();
    trie_bldr.set_out_vec(output_buf);
    trie_bldr.write_kv();
    trie_bldr.close_file();
    auto trie = std::make_unique<trie_t>();
    trie->load_from_mem(output_buf->data(), output_buf->size());
    cleanup_madras *cleanup_obj = new cleanup_madras();
    trie->set_cleanup_object(cleanup_obj);
    return trie;
}
template <>
uint64_t get_memory(trie_t* trie) {
    return trie->get_size();
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    static madras_dv1::input_ctx in_ctx;
    in_ctx.key = (const uint8_t *) query.c_str();
    in_ctx.key_len = query.length();
    trie->lookup(in_ctx);
    return trie->leaf_rank1(in_ctx.node_id);
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    uint8_t out_key[trie->get_max_key_len()];
    size_t out_key_len;
    trie->reverse_lookup(query, &out_key_len, out_key);
    return out_key_len;
}
#endif

#ifdef USE_ART
#include <art.hpp>
using trie_t = art::art_trie;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    auto trie = std::make_unique<trie_t>();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        trie->art_insert((const uint8_t *) keys[i].c_str(), keys[i].length(), nullptr);
    }
    return trie;
}
template <>
uint64_t get_memory(trie_t* trie) {
    return trie->art_size_in_bytes();
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    trie->art_search((const uint8_t *) query.c_str(), query.length());
    return 0;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return 0;
}
#endif

#ifdef USE_XCDAT_7
#include <xcdat.hpp>
using trie_t = xcdat::trie_7_type;
#endif
#ifdef USE_XCDAT_8
#include <xcdat.hpp>
using trie_t = xcdat::trie_8_type;
#endif
#ifdef USE_XCDAT_15
#include <xcdat.hpp>
using trie_t = xcdat::trie_15_type;
#endif
#ifdef USE_XCDAT_16
#include <xcdat.hpp>
using trie_t = xcdat::trie_16_type;
#endif

#if defined(USE_XCDAT_7) || defined(USE_XCDAT_8) || defined(USE_XCDAT_15) || defined(USE_XCDAT_16)
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    auto trie = std::make_unique<trie_t>(keys);
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    return trie->lookup(query).value_or(NOT_FOUND);
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    static std::string ret;
    trie->decode(query, ret);
    return ret.size();
}
template <>
uint64_t get_memory(trie_t* trie) {
    xcdat::save(*trie, TMP_INDEX_FILENAME);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_PDT
#include <succinct/mapper.hpp>
#include <tries/compressed_string_pool.hpp>
#include <tries/path_decomposed_trie.hpp>
using trie_t = succinct::tries::path_decomposed_trie<succinct::tries::compressed_string_pool>;
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    auto trie = std::make_unique<trie_t>(keys);
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    auto res = trie->index(query);
    return res != static_cast<size_t>(-1) ? uint64_t(res) : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return trie->operator[](query).size();
}
template <>
uint64_t get_memory(trie_t* trie) {
    succinct::mapper::freeze(*trie, TMP_INDEX_FILENAME);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

#ifdef USE_HATTRIE
#include <tsl/htrie_map.h>
using trie_t = tsl::htrie_map<char, uint32_t>;
#endif
#ifdef USE_ARRAYHASH
#include <tsl/array_map.h>
using trie_t = tsl::array_map<char, uint32_t>;  // although this is not trie...
#endif

#if defined(USE_HATTRIE) || defined(USE_ARRAYHASH)
class serializer {
  public:
    serializer(const char* file_name) {
        m_ostream.exceptions(m_ostream.badbit | m_ostream.failbit);
        m_ostream.open(file_name);
    }
    template <class T, typename std::enable_if<std::is_arithmetic<T>::value>::type* = nullptr>
    void operator()(const T& value) {
        m_ostream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    void operator()(const char* value, std::size_t value_size) {
        m_ostream.write(value, value_size);
    }

  private:
    std::ofstream m_ostream;
};
template <>
std::unique_ptr<trie_t> build(std::vector<std::string>& keys, build_opts& opts) {
    auto trie = std::make_unique<trie_t>();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        trie->insert(keys[i].c_str(), uint32_t(i));
    }
    return trie;
}
template <>
uint64_t lookup(trie_t* trie, const std::string& query) {
    auto it = trie->find(query.c_str());
    return it != trie->end() ? *it : NOT_FOUND;
}
template <>
uint64_t decode(trie_t* trie, uint64_t query) {
    return 0;
}
template <>
uint64_t get_memory(trie_t* trie) {
    serializer serial(TMP_INDEX_FILENAME);
    trie->serialize(serial);
    return essentials::file_size(TMP_INDEX_FILENAME);
}
#endif

template <class T>
void main_template(const char* title, std::vector<std::string>& keys, std::vector<std::string>& queries,
                   bool run_decode, build_opts& opts) {
    essentials::json_lines logger;
    logger.add("name", title);

    std::unique_ptr<T> trie;
    {
        essentials::timer<essentials::clock_type, std::chrono::nanoseconds> tm;
        tm.start();
        trie = build<T>(keys, opts);
        tm.stop();
        logger.add("build_ns_per_key", tm.average() / keys.size());
    }

    {
        essentials::timer<essentials::clock_type, std::chrono::nanoseconds> tm;
        for (int i = 0; i <= SEARCH_RUNS; ++i) {
            tm.start();
            for (const auto& query : queries) {
                if (lookup(trie.get(), query) == NOT_FOUND) {
                    tfm::errorfln("Not found: %s", query);
                    return;
                }
            }
            tm.stop();
        }
        tm.discard_first();  // for warming up
        logger.add("lookup_ns_per_query", tm.average() / queries.size());
        logger.add("best_lookup_ns_per_query", tm.min() / queries.size());
    }

    if (run_decode) {
        std::vector<uint64_t> ids(queries.size());
        for (size_t i = 0; i < queries.size(); i++) {
            ids[i] = lookup(trie.get(), queries[i]);
        }

        essentials::timer<essentials::clock_type, std::chrono::nanoseconds> tm;
        for (int i = 0; i <= SEARCH_RUNS; ++i) {
            tm.start();
            for (const auto id : ids) {
                if (decode(trie.get(), id) == 0) {
                    tfm::errorfln("Not found: %d", id);
                    return;
                }
            }
            tm.stop();
        }
        tm.discard_first();  // for warming up
        logger.add("decode_ns_per_query", tm.average() / ids.size());
        logger.add("best_decode_ns_per_query", tm.min() / ids.size());
    }

    const uint64_t mem = get_memory(trie.get());
    logger.add("memory_in_bytes", mem);

    logger.print();
}

cmd_line_parser::parser make_parser(int argc, char** argv) {
    cmd_line_parser::parser p(argc, argv);
    p.add("input_keys", "Input filepath of keywords");
    p.add("num_samples", "Number of sample keys for searches (default=100000)", "-n", false);
    p.add("random_seed", "Random seed for sampling (default=13)", "-s", false);
    p.add("to_unique", "Unique strings? (default=false)", "-u", false);
    p.add("force_asc", "Force Ascending label order? (default=false)", "-a", false);
    return p;
}

int main(int argc, char* argv[]) {
#ifndef NDEBUG
    tfm::warnfln("The code is running in debug mode.");
#endif
    std::ios::sync_with_stdio(false);

    auto p = make_parser(argc, argv);
    if (!p.parse()) {
        return 1;
    }

    const auto input_keys = p.get<std::string>("input_keys");
    const auto num_samples = p.get<std::uint64_t>("num_samples", 100000);
    const auto random_seed = p.get<std::uint64_t>("random_seed", 13);
    const auto to_unique = p.get<bool>("to_unique", false);
    const auto force_asc = p.get<bool>("force_asc", false);

    build_opts opts;
    opts.force_asc = force_asc;

    auto keys = load_strings(input_keys, to_unique);
    auto queries = sample_strings(keys, num_samples, random_seed);

#ifdef USE_FST
    main_template<trie_t>("FST", keys, queries, false, opts);
#endif
#ifdef USE_DARTS
    main_template<trie_t>("DARTS", keys, queries, false, opts);
#endif
#ifdef USE_DARTSC
    main_template<trie_t>("DARTSC", keys, queries, false, opts);
#endif
#ifdef USE_CEDAR
    main_template<trie_t>("CEDAR", keys, queries, false, opts);
#endif
#ifdef USE_CEDARPP
    main_template<trie_t>("CEDARPP", keys, queries, false, opts);
#endif
#ifdef USE_DASTRIE
    main_template<trie_t>("DASTRIE", keys, queries, false, opts);
#endif
#ifdef USE_TX
    main_template<trie_t>("TX", keys, queries, true, opts);
#endif
#ifdef USE_MARISA
    main_template<trie_t>("MARISA", keys, queries, true, opts);
#endif
#ifdef USE_MADRAS
    main_template<trie_t>("MADRAS", keys, queries, true, opts);
#endif
#ifdef USE_ART
    main_template<trie_t>("ART", keys, queries, false, opts);
#endif
#ifdef USE_XCDAT_7
    main_template<trie_t>("XCDAT_7", keys, queries, true, opts);
#endif
#ifdef USE_XCDAT_8
    main_template<trie_t>("XCDAT_8", keys, queries, true, opts);
#endif
#ifdef USE_XCDAT_15
    main_template<trie_t>("XCDAT_15", keys, queries, true, opts);
#endif
#ifdef USE_XCDAT_16
    main_template<trie_t>("XCDAT_16", keys, queries, true, opts);
#endif
#ifdef USE_PDT
    main_template<trie_t>("PDT", keys, queries, true, opts);
#endif
#ifdef USE_HATTRIE
    main_template<trie_t>("HATTRIE", keys, queries, false, opts);
#endif
#ifdef USE_ARRAYHASH
    main_template<trie_t>("ARRAYHASH", keys, queries, false, opts);
#endif
    std::remove(TMP_INDEX_FILENAME);

    return 0;
}
