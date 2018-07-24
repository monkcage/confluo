#ifndef CONFLUO_CONTAINER_SKETCH_CONFLUO_UNIVERSAL_SKETCH_H
#define CONFLUO_CONTAINER_SKETCH_CONFLUO_UNIVERSAL_SKETCH_H

#include <vector>

#include "atomic.h"
#include "count_sketch.h"
#include "hash_manager.h"
#include "priority_queue.h"

namespace confluo {
namespace sketch {

template<typename counter_t = int64_t>
class confluo_substream_summary {

public:
  typedef atomic::type<counter_t> atomic_counter_t;
  typedef std::vector<atomic::type<size_t>> atomic_vector_t;
  typedef count_sketch<counter_t> sketch_t;
  typedef heavy_hitter_set<size_t, counter_t> heavy_hitter_set_t;

  confluo_substream_summary() = default;

  /**
   * Constructor
   * @param t depth (number of estimates)
   * @param b width (number of buckets)
   * @param k number of heavy hitters to track
   * @param a heavy hitter threshold
   * @param precise track exact heavy hitters
   */
  confluo_substream_summary(size_t t, size_t b, size_t k, double a, bool precise = true)
          : hh_threshold_(a),
            num_hh_(k),
            l2_squared_(),
            sketch_(t, b),
            heavy_hitters_(k),
            hhs_precise_(),
            hh_hash_(pairwise_indep_hash::generate_random()),
            use_precise_hh_(precise) {
  }

  confluo_substream_summary(const confluo_substream_summary& other)
          : hh_threshold_(other.hh_threshold_),
            num_hh_(other.num_hh_),
            l2_squared_(atomic::load(&other.l2_squared_)),
            sketch_(other.sketch_),
            heavy_hitters_(other.heavy_hitters_.size()),
            hhs_precise_(other.hhs_precise_),
            hh_hash_(other.hh_hash_),
            use_precise_hh_(other.use_precise_hh_) {
    for (size_t i = 0; i < other.heavy_hitters_.size(); i++) {
      atomic::store(&heavy_hitters_[i], atomic::load(&other.heavy_hitters_[i]));
    }
  }

  confluo_substream_summary& operator=(const confluo_substream_summary& other) {
    hh_threshold_ = other.hh_threshold_;
    num_hh_ = other.num_hh_;
    l2_squared_ = atomic::load(&other.l2_squared_);
    sketch_ = other.sketch_;
    heavy_hitters_ = atomic_vector_t(other.heavy_hitters_.size());
    hhs_precise_ = other.hhs_precise_;
    hh_hash_ = other.hh_hash_;
    use_precise_hh_ = other.use_precise_hh_;
    for (size_t i = 0; i < other.heavy_hitters_.size(); i++) {
      atomic::store(&heavy_hitters_[i], atomic::load(&other.heavy_hitters_[i]));
    }
    return *this;
  }

  void update(size_t key_hash) {
    counter_t old_count = sketch_.update_and_estimate(key_hash);
    counter_t update = l2_squared_update(old_count);
    counter_t old_l2_sq = atomic::faa(&l2_squared_, update);
    double new_l2 = std::sqrt(old_l2_sq + update);
    if (use_precise_hh_) {
      this->update_hh_pq(key_hash, old_count + 1, new_l2);
    } else {
      this->update_hh_approx(key_hash, old_count + 1, new_l2);
    }
  }

  /**
   * Estimate count
   * @param key key
   * @return estimated count
   */
  counter_t estimate(const byte_string& key) {
    return sketch_.estimate(key);
  }

  /**
   * @return sketch
   */
  sketch_t& get_sketch() {
    return sketch_;
  }

  atomic_vector_t& get_heavy_hitters() {
    return heavy_hitters_;
  }

  heavy_hitter_set_t& get_pq() {
    return hhs_precise_;
  }

  /**
   * @return size of data structure in bytes
   */
  size_t storage_size() {
    size_t total_size = 0;
    total_size += sketch_.storage_size();
    total_size += heavy_hitters_.size();
    return total_size;
  }

private:
  /**
   * Update heavy hitters priority queue
   * @param key_hash key hash
   * @param count frequency count
   * @param l2 current l2 norm
   */
  void update_hh_pq(size_t key_hash, counter_t count, double l2) {
    if (count < hh_threshold_ * l2) {
      return;
    }
    if (hhs_precise_.size() < num_hh_) {
      hhs_precise_.remove_if_exists(key_hash);
      hhs_precise_.pushp(key_hash, count);
    } else {
      auto head = hhs_precise_.top().key_;
      if (sketch_.estimate(head) < count) {
        hhs_precise_.pop();
        hhs_precise_.remove_if_exists(key_hash);
        hhs_precise_.pushp(key_hash, count);
      }
    }
  }

  /**
   * Update heavy hitters approximate DS
   * @param key_hash key hash
   * @param count frequency count
   * @param l2 current l2 norm
   */
  void update_hh_approx(size_t key_hash, counter_t count, double l2) {
    if (count < hh_threshold_ * l2) {
      return;
    }
    bool done = false;
    while (!done) {
      size_t idx = hh_hash_.apply<size_t>(key_hash) % heavy_hitters_.size();
      size_t prev_key_hash = atomic::load(&heavy_hitters_[idx]);
      if (prev_key_hash == key_hash)
        return;
      counter_t prev_count = sketch_.estimate(prev_key_hash);
      done = (prev_count > count) ? true : atomic::strong::cas(&heavy_hitters_[idx], &prev_key_hash, key_hash);
    }
  }

  /**
   * L_2^2 += (c_i + 1)^2 - (c_i)^2
   * @param old_count estimate of a count before an update
   */
  static inline counter_t l2_squared_update(counter_t old_count) {
    return 2 * old_count + 1;
  }

  double hh_threshold_; // heavy hitter threshold
  size_t num_hh_; // number of heavy hitters to track (k)

  atomic_counter_t l2_squared_; // L2 norm squared
  sketch_t sketch_;
  atomic_vector_t heavy_hitters_;
  heavy_hitter_set_t hhs_precise_;
  pairwise_indep_hash hh_hash_;

  bool use_precise_hh_;

};

template<typename counter_t = int64_t>
class confluo_universal_sketch {

public:
  typedef std::vector<atomic::type<size_t>> heavy_hitters_set;
  template<typename g_ret_t> using g_fn = std::function<g_ret_t(counter_t)>;

  /**
   * Constructor
   * @param l number of layers
   * @param t count-sketch depth (number of estimates)
   * @param b count-sketch width (number of buckets)
   * @param k number of heavy hitters to track per layer
   * @param a heavy hitter threshold
   * @param precise track exact heavy hitters
   */
  confluo_universal_sketch(size_t l, size_t t, size_t b, size_t k, double a,
                           const schema_t& schema, const column_t& column,
                           bool precise = true)
          : substream_summaries_(l),
            layer_hashes_(l - 1),
            schema_(schema),
            column_(column),
            precise_hh_(precise),
            is_valid_(true) {
    layer_hashes_.guarantee_initialized(l - 1);
    for (size_t i = 0; i < l; i++) {
      substream_summaries_[i] = confluo_substream_summary<counter_t>(t, b, k, a, precise);
    }
  }

  confluo_universal_sketch(const confluo_universal_sketch& other)
          : substream_summaries_(other.substream_summaries_),
            layer_hashes_(other.layer_hashes_),
            schema_(other.schema_),
            column_(other.column_),
            precise_hh_(other.precise_hh_),
            is_valid_(atomic::load(&other.is_valid_)) {
  }

  confluo_universal_sketch& operator=(const confluo_universal_sketch& other) {
    substream_summaries_ = other.substream_summaries_;
    layer_hashes_ = other.layer_hashes_;
    schema_= other.schema_;
    column_ = other.column_;
    precise_hh_ = other.precise_hh_;
    is_valid_ = atomic::load(&other.is_valid_);
    return *this;
  }

  bool is_valid() {
    return atomic::load(&is_valid_);
  }

  bool invalidate() {
    bool expected = true;
    return atomic::strong::cas(&is_valid_, &expected, false);
  }

  /**
   * Update universal sketch with a record.
   * @param r record
   */
  void update(const record_t &r) {
    size_t key_hash = r.at(column_.idx()).get_key().hash();
    substream_summaries_[0].update(key_hash);
    for (size_t i = 1; i < substream_summaries_.size() && to_bool(layer_hashes_.hash(i - 1, key_hash)); i++) {
      substream_summaries_[i].update(key_hash);
    }
  }

  /**
   * Evaluate a G_SUM function using all layers
   * @tparam g_ret_t return type
   * @param g function
   * @return g sum estimate
   */
  template<typename g_ret_t = counter_t>
  g_ret_t evaluate(g_fn<g_ret_t> g) {
    return evaluate(g, substream_summaries_.size());
  }

  /**
   * Evaluate a G_SUM function
   * @tparam g_ret_t return type
   * @param g function
   * @param nlayers number of layers to use
   * @return g sum
   */
  template<typename g_ret_t = counter_t>
  g_ret_t evaluate(g_fn<g_ret_t> g, size_t nlayers) {
    g_ret_t recursive_sum = 0;

    // Handle last substream
    size_t substream_i = nlayers - 1;

    auto& last_substream_sketch = substream_summaries_[substream_i].get_sketch();

    if (precise_hh_) {
      auto& last_substream_hhs = substream_summaries_[substream_i].get_pq();
      for (auto it = last_substream_hhs.begin(); it != last_substream_hhs.end(); ++it) {
        counter_t count = (*it).priority_;
        recursive_sum += g(count);
      }
      //LOG_INFO << substream_i << ": " << recursive_sum;
    }
    else {
      auto& last_substream_hhs = substream_summaries_[substream_i].get_heavy_hitters();
      for (size_t hh_i = 0; hh_i < last_substream_hhs.size(); hh_i++) {
        auto hh_rec_off = atomic::load(&last_substream_hhs[hh_i]);
        // TODO handle special case
        if (hh_rec_off != 0) {
          size_t hh_hash = 0;
          counter_t count = last_substream_sketch.estimate(hh_hash);
          recursive_sum += g(count);
        }
      }
    }

    // Handle rest recursively

    while (substream_i-- > 0) {

      g_ret_t substream_sum = 0;
      auto& substream_sketch = substream_summaries_[substream_i].get_sketch();

      if (precise_hh_) {
        auto& substream_hhs = substream_summaries_[substream_i].get_pq();
        for (auto it = substream_hhs.begin(); it != substream_hhs.end(); ++it) {
          size_t hh_hash = (*it).key_;
          counter_t count = (*it).priority_;
          g_ret_t update = ((1 - 2 * to_bool(layer_hashes_.hash(substream_i, hh_hash))) * g(count));
          substream_sum += update;
        }
      }
      else {
        auto &substream_hhs = substream_summaries_[substream_i].get_heavy_hitters();
        for (size_t hh_i = 0; hh_i < substream_hhs.size(); hh_i++) {
          auto hh_hash = atomic::load(&substream_hhs[hh_i]);
          counter_t count = substream_sketch.estimate(hh_hash);
          // TODO handle special case
          if (hh_hash != byte_string().hash()) {
            g_ret_t update = ((1 - 2 * to_bool(layer_hashes_.hash(substream_i, hh_hash))) * g(count));
            substream_sum += update;
          }
        }
      }

      recursive_sum = 2 * recursive_sum + substream_sum;
      //LOG_INFO << substream_i << ": " << recursive_sum;
    }
    return recursive_sum;
  }

  /**
   * @return size of data structure in bytes
   */
  size_t storage_size() {
    size_t total_size = 0;
    for (size_t i = 0; i < substream_summaries_.size(); i++) {
      total_size += substream_summaries_[i].storage_size();
    }
    return total_size;
  }

  static confluo_universal_sketch<counter_t> create_parameterized(double epsilon, double gamma, size_t k, double a,
                                                                  const schema_t& schema, const column_t& column) {
    size_t nlayers = 8 * sizeof(column.type().size);
    return {
            nlayers,
            count_sketch<counter_t>::error_margin_to_width(epsilon),
            count_sketch<counter_t>::perror_to_depth(gamma),
            k, a, schema, column
    };
  }

private:
  static inline size_t to_bool(size_t hashed_value) {
    return hashed_value % 2;
  }

  std::vector<confluo_substream_summary<counter_t>> substream_summaries_;
  hash_manager<size_t> layer_hashes_;

  schema_t schema_;
  column_t column_;

  bool precise_hh_;
  atomic::type<bool> is_valid_;

};

}
}

#endif //CONFLUO_CONTAINER_SKETCH_CONFLUO_UNIVERSAL_SKETCH_H
