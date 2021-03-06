// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <cassert>
#include <cstring>
#include <random>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include "crimson/common/log.h"
#include "stages/key_layout.h"
#include "tree.h"

/**
 * tree_utils.h
 *
 * Contains shared logic for unit tests and perf tool.
 */

namespace crimson::os::seastore::onode {

class Onodes {
 public:
  Onodes(size_t n) {
    for (size_t i = 1; i <= n; ++i) {
      auto p_onode = &create(i * 8);
      onodes.push_back(p_onode);
    }
  }

  Onodes(std::vector<size_t> sizes) {
    for (auto& size : sizes) {
      auto p_onode = &create(size);
      onodes.push_back(p_onode);
    }
  }

  ~Onodes() = default;

  const onode_t& create(size_t size) {
    ceph_assert(size <= std::numeric_limits<uint16_t>::max());
    onode_t config{static_cast<uint16_t>(size), id++};
    auto onode = onode_t::allocate(config);
    auto p_onode = onode.get();
    tracked_onodes.push_back(std::move(onode));
    return *reinterpret_cast<onode_t*>(p_onode);
  }

  const onode_t& pick() const {
    auto index = rd() % onodes.size();
    return *onodes[index];
  }

  const onode_t& pick_largest() const {
    return *onodes[onodes.size() - 1];
  }

  static void validate_cursor(
      const Btree::Cursor& cursor, const ghobject_t& key, const onode_t& onode) {
    ceph_assert(!cursor.is_end());
    ceph_assert(cursor.get_ghobj() == key);
    ceph_assert(cursor.value());
    ceph_assert(cursor.value() != &onode);
    ceph_assert(*cursor.value() == onode);
    onode_t::validate_tail_magic(*cursor.value());
  }

 private:
  uint16_t id = 0;
  mutable std::random_device rd;
  std::vector<const onode_t*> onodes;
  std::vector<std::unique_ptr<char[]>> tracked_onodes;
};

class KVPool {
  struct kv_conf_t {
    unsigned index2;
    unsigned index1;
    unsigned index0;
    size_t ns_size;
    size_t oid_size;
    const onode_t* p_value;

    ghobject_t get_ghobj() const {
      assert(index1 < 10);
      std::ostringstream os_ns;
      os_ns << "ns" << index1;
      unsigned current_size = (unsigned)os_ns.tellp();
      assert(ns_size >= current_size);
      os_ns << std::string(ns_size - current_size, '_');

      std::ostringstream os_oid;
      os_oid << "oid" << index1;
      current_size = (unsigned)os_oid.tellp();
      assert(oid_size >= current_size);
      os_oid << std::string(oid_size - current_size, '_');

      return ghobject_t(shard_id_t(index2), index2, index2,
                        os_ns.str(), os_oid.str(), index0, index0);
    }
  };
  using kv_vector_t = std::vector<kv_conf_t>;

 public:
  using kv_t = std::pair<ghobject_t, const onode_t*>;

  KVPool(const std::vector<size_t>& str_sizes,
         const std::vector<size_t>& onode_sizes,
         const std::pair<unsigned, unsigned>& range2,
         const std::pair<unsigned, unsigned>& range1,
         const std::pair<unsigned, unsigned>& range0)
      : str_sizes{str_sizes}, onodes{onode_sizes} {
    ceph_assert(range2.first < range2.second);
    ceph_assert(range2.second - 1 <= (unsigned)std::numeric_limits<shard_t>::max());
    ceph_assert(range2.second - 1 <= std::numeric_limits<crush_hash_t>::max());
    ceph_assert(range1.first < range1.second);
    ceph_assert(range1.second - 1 <= 9);
    ceph_assert(range0.first < range0.second);
    std::random_device rd;
    for (unsigned i = range2.first; i < range2.second; ++i) {
      for (unsigned j = range1.first; j < range1.second; ++j) {
        auto ns_size = (unsigned)str_sizes[rd() % str_sizes.size()];
        auto oid_size = (unsigned)str_sizes[rd() % str_sizes.size()];
        for (unsigned k = range0.first; k < range0.second; ++k) {
          kvs.emplace_back(kv_conf_t{i, j, k, ns_size, oid_size, &onodes.pick()});
        }
      }
    }
    random_kvs = kvs;
    std::random_shuffle(random_kvs.begin(), random_kvs.end());
  }

  class iterator_t {
   public:
    iterator_t() = default;
    iterator_t(const iterator_t&) = default;
    iterator_t(iterator_t&&) = default;
    iterator_t& operator=(const iterator_t&) = default;
    iterator_t& operator=(iterator_t&&) = default;

    kv_t get_kv() const {
      assert(!is_end());
      auto& conf = (*p_kvs)[i];
      return std::make_pair(conf.get_ghobj(), conf.p_value);
    }
    bool is_end() const { return !p_kvs || i >= p_kvs->size(); }
    size_t index() const { return i; }

    iterator_t& operator++() {
      assert(!is_end());
      ++i;
      return *this;
    }

    iterator_t operator++(int) {
      iterator_t tmp = *this;
      ++*this;
      return tmp;
    }

   private:
    iterator_t(const kv_vector_t& kvs) : p_kvs{&kvs} {}

    const kv_vector_t* p_kvs = nullptr;
    size_t i = 0;
    friend class KVPool;
  };

  iterator_t begin() const {
    return iterator_t(kvs);
  }

  iterator_t random_begin() const {
    return iterator_t(random_kvs);
  }

  size_t size() const {
    return kvs.size();
  }

 private:
  std::vector<size_t> str_sizes;
  Onodes onodes;
  kv_vector_t kvs;
  kv_vector_t random_kvs;
};

template <bool TRACK>
class TreeBuilder {
 public:
  using ertr = Btree::btree_ertr;
  template <class ValueT=void>
  using future = ertr::future<ValueT>;

  TreeBuilder(KVPool& kvs, NodeExtentManagerURef&& nm)
      : kvs{kvs} {
    tree.emplace(std::move(nm));
  }

  future<> bootstrap(Transaction& t) {
    std::ostringstream oss;
#ifndef NDEBUG
    oss << "debug=on, ";
#else
    oss << "debug=off, ";
#endif
#ifdef UNIT_TESTS_BUILT
    oss << "UNIT_TEST_BUILT=on, ";
#else
    oss << "UNIT_TEST_BUILT=off, ";
#endif
    if constexpr (TRACK) {
      oss << "track=on, ";
    } else {
      oss << "track=off, ";
    }
    oss << *tree;
    logger().warn("TreeBuilder: {}, bootstrapping ...", oss.str());
    return tree->mkfs(t);
  }

  future<> insert(Transaction& t) {
    kv_iter = kvs.random_begin();
    auto cursors = seastar::make_lw_shared<std::vector<Btree::Cursor>>();
    logger().warn("start inserting {} kvs ...", kvs.size());
    auto start_time = mono_clock::now();
    return crimson::do_until([&t, this, cursors]() -> future<bool> {
      if (kv_iter.is_end()) {
        return ertr::make_ready_future<bool>(true);
      }
      auto [key, p_value] = kv_iter.get_kv();
      logger().debug("[{}] {} -> {}", kv_iter.index(), key_hobj_t{key}, *p_value);
      return tree->insert(t, key, *p_value
      ).safe_then([&t, this, cursors](auto ret) {
        auto& [cursor, success] = ret;
        assert(success == true);
        if constexpr (TRACK) {
          cursors->emplace_back(cursor);
        }
#ifndef NDEBUG
        auto [key, p_value] = kv_iter.get_kv();
        Onodes::validate_cursor(cursor, key, *p_value);
        return tree->lower_bound(t, key).safe_then([this, cursor](auto cursor_) {
          auto [key, p_value] = kv_iter.get_kv();
          ceph_assert(cursor_.get_ghobj() == key);
          ceph_assert(cursor_.value() == cursor.value());
          ++kv_iter;
          return ertr::make_ready_future<bool>(false);
        });
#else
        ++kv_iter;
        return ertr::make_ready_future<bool>(false);
#endif
      });
    }).safe_then([&t, this, start_time, cursors] {
      std::chrono::duration<double> duration = mono_clock::now() - start_time;
      logger().warn("Insert done! {}s", duration.count());
      if (!cursors->empty()) {
        logger().info("Verifing tracked cursors ...");
        kv_iter = kvs.random_begin();
        return seastar::do_with(
            cursors->begin(), [&t, this, cursors](auto& c_iter) {
          return crimson::do_until([&t, this, &c_iter, cursors]() -> future<bool> {
            if (kv_iter.is_end()) {
              logger().info("Verify done!");
              return ertr::make_ready_future<bool>(true);
            }
            assert(c_iter != cursors->end());
            auto [k, v] = kv_iter.get_kv();
            // validate values in tree keep intact
            return tree->lower_bound(t, k).safe_then([this, &c_iter](auto cursor) {
              auto [k, v] = kv_iter.get_kv();
              Onodes::validate_cursor(cursor, k, *v);
              // validate values in cursors keep intact
              Onodes::validate_cursor(*c_iter, k, *v);
              ++kv_iter;
              ++c_iter;
              return ertr::make_ready_future<bool>(false);
            });
          });
        });
      } else {
        return ertr::now();
      }
    });
  }

  future<> get_stats(Transaction& t) {
    return tree->get_stats_slow(t
    ).safe_then([this](auto stats) {
      logger().warn("{}", stats);
    });
  }

  void reload(NodeExtentManagerURef&& nm) {
    tree.emplace(std::move(nm));
  }

  future<> validate(Transaction& t) {
    logger().info("Verifing insertion ...");
    return seastar::do_with(
        kvs.begin(), [&t, this] (auto& kvs_iter) {
      return crimson::do_until([&t, this, &kvs_iter]() -> future<bool> {
        if (kvs_iter.is_end()) {
          logger().info("Verify done!");
          return ertr::make_ready_future<bool>(true);
        }
        auto [k, v] = kvs_iter.get_kv();
        return tree->lower_bound(t, k
        ).safe_then([&kvs_iter, k=k, v=v] (auto cursor) {
          Onodes::validate_cursor(cursor, k, *v);
          ++kvs_iter;
          return ertr::make_ready_future<bool>(false);
        });
      });
    });
  }

 private:
  static seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_filestore);
  }

  KVPool& kvs;
  std::optional<Btree> tree;
  KVPool::iterator_t kv_iter;
};

}
