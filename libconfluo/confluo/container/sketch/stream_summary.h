#ifndef CONFLUO_CONTAINER_SKETCH_SUBSTREAM_SUMMARY_H
#define CONFLUO_CONTAINER_SKETCH_SUBSTREAM_SUMMARY_H

#include <vector>

#include "atomic.h"
#include "count_sketch.h"
#include "hash_manager.h"
#include "priority_queue.h"

namespace confluo {
namespace sketch {

template<typename T, typename counter_t = int64_t>
class stream_summary {

public:
  typedef atomic::type<counter_t> atomic_counter_t;
  typedef std::vector<atomic::type<T>> atomic_vector_t;
  typedef count_sketch<T, counter_t> sketch_t;

  stream_summary() = default;

  /**
   * Constructor
   * @param b width (number of buckets)
   * @param t depth (number of estimates)
   * @param k number of heavy hitters to track
   * @param m1 sketch's hash manager for buckets
   * @param m2 sketch's hash manager for signs
   * @param pwih hash function for heavy hitter approximation
   */
  stream_summary(size_t b, size_t t, size_t k, hash_manager m1, hash_manager m2, pairwise_indep_hash pwih)
      : num_hh_(k),
        l2_squared_(),
        sketch_(b, t, m1, m2),
        heavy_hitters_(k),
        hhs_precise_(),
        hh_hash_(pwih),
        use_precise_hh_(true) {
  }

  /**
   * Constructor
   * @param b width (number of buckets)
   * @param t depth (number of estimates)
   * @param k number of heavy hitters to track
   * @param precise track exact heavy hitters
   */
  stream_summary(size_t b, size_t t, size_t k, bool precise = true)
      : num_hh_(k),
        l2_squared_(),
        sketch_(b, t),
        heavy_hitters_(k),
        hhs_precise_(),
        hh_hash_(pairwise_indep_hash::generate_random()),
        use_precise_hh_(precise) {
  }

  stream_summary(const stream_summary& other)
      : num_hh_(other.num_hh_),
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

  stream_summary& operator=(const stream_summary& other) {
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

  void update(T key, size_t incr = 1) {
    counter_t old_count = sketch_.update_and_estimate(key, incr);
    if (use_precise_hh_) {
      this->update_hh_pq(key, old_count + incr);
    } else {
      this->update_hh_approx(key, old_count + incr);
    }
  }

  /**
   * Estimate count
   * @param key key
   * @return estimated count
   */
  counter_t estimate(T key) {
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

  pq<T, counter_t>& get_pq() {
    return hhs_precise_;
  }

  /**
   * @return size of data structure in bytes
   */
  size_t storage_size() {
    size_t total_size = 0;
    total_size += sketch_.storage_size();
    if (use_precise_hh_)
      total_size += hhs_precise_.storage_size();
    else
      total_size += heavy_hitters_.size();
    return total_size;
  }

private:
  /**
   * Update heavy hitters priority queue
   * TODO make thread-safe
   * @param key key
   * @param count frequency count
   */
  void update_hh_pq(T key, counter_t count) {
    if (hhs_precise_.size() < num_hh_) {
      hhs_precise_.update(key, count, true);
    }
    else {
      // Update the key only if it already exists
      bool updated = hhs_precise_.update(key, count, false);
      // Insert the key if it didn't exist and has greater priority than the current min
      if (!updated && sketch_.estimate(hhs_precise_.top().key) < count) {
        // This uses an up-to-date frequency of the head element.
        // As an optimization we can use the stale value (head.priority)
        hhs_precise_.pop();
        hhs_precise_.pushp(key, count);
      }
    }
  }

  /**
   * Update heavy hitters approximate DS
   * @param key key
   * @param count frequency count
   */
  void update_hh_approx(T key, counter_t count) {
    bool done = false;
    while (!done) {
      size_t idx = hh_hash_.apply<T>(key) % heavy_hitters_.size();
      T prev = atomic::load(&heavy_hitters_[idx]);
      if (prev == key)
        return;
      counter_t prev_count = sketch_.estimate(prev);
      done = (prev_count > count) ? true : atomic::strong::cas(&heavy_hitters_[idx], &prev, key);
    }
  }

  size_t num_hh_; // number of heavy hitters to track (k)

  atomic_counter_t l2_squared_; // L2 norm squared
  sketch_t sketch_;
  atomic_vector_t heavy_hitters_;
  pq<T, counter_t> hhs_precise_;
  pairwise_indep_hash hh_hash_;

  bool use_precise_hh_;

};

}
}

#endif // CONFLUO_CONTAINER_SKETCH_SUBSTREAM_SUMMARY_H
