#include "transclosure.hpp"

namespace seqwish {

using namespace std::chrono_literals;

void extend_range(const uint64_t& s_pos,
                  const pos_t& q_pos,
                  std::map<pos_t, std::pair<uint64_t, uint64_t>>& range_buffer) {
    // find a position in the map that we can add onto
    // it must match position and orientation
    pos_t q_last_pos = q_pos;
    decr_pos(q_last_pos);
    auto f = range_buffer.find(q_last_pos);
    // if one doesn't exist, add the range
    if (f == range_buffer.end()) {
        range_buffer[q_pos] = std::make_pair(s_pos, 1);
    } else {
        // if one does, check that it matches our extension,
        std::pair<uint64_t, uint64_t> x = f->second;
        if (x.first + x.second == s_pos) {
            // if so we expand its range and drop it back into the map at the new Q end pos
            range_buffer.erase(f);
            ++x.second; // increment the match length
            range_buffer[q_pos] = x; // and stash it
        } else {
            // if it doesn't, we store a new range
            range_buffer[q_pos] = std::make_pair(s_pos, 1);
        }
    }
}

void flush_ranges(const uint64_t& s_pos,
                  std::map<pos_t, std::pair<uint64_t, uint64_t>>& range_buffer,
                  mmmulti::iitree<uint64_t, pos_t>& node_iitree,
                  mmmulti::iitree<uint64_t, pos_t>& path_iitree) {
    // for each range, we're going to see if we've stepped more than one past the end
    // if we have, we'll write them out
    std::map<pos_t, std::pair<uint64_t, uint64_t>>::iterator it = range_buffer.begin();
    while (it != range_buffer.end()) {
        if (it->second.first + it->second.second != s_pos) {
            uint64_t match_length, match_start_in_s, match_end_in_s, match_start_in_q, match_end_in_q;
            pos_t match_pos_in_q, match_pos_in_s, match_start_pos_in_q;
            pos_t match_end_pos_in_q = it->first;
            bool is_rev_match = is_rev(match_end_pos_in_q);
            if (!is_rev_match) {
                match_length = it->second.second;
                match_start_in_s = it->second.first;
                match_end_in_s = match_start_in_s + match_length;
                match_end_in_q = offset(match_end_pos_in_q) + 1;
                match_start_in_q = match_end_in_q - match_length;
                match_pos_in_s = make_pos_t(match_start_in_s, false);
                match_pos_in_q = make_pos_t(match_start_in_q, false);
            } else {
                match_length = it->second.second;
                match_start_in_s = it->second.first;
                match_end_in_s = match_start_in_s + match_length;
                match_end_in_q = offset(match_end_pos_in_q);
                decr_pos(match_end_pos_in_q, match_length);
                match_pos_in_s = make_pos_t(match_end_in_s-1, true);
                match_pos_in_q = make_pos_t(offset(match_end_pos_in_q)-1, true);
                match_start_in_q = match_end_in_q;
                match_end_in_q = offset(match_end_pos_in_q);
            }
            node_iitree.add(match_start_in_s, match_end_in_s, match_pos_in_q);
            path_iitree.add(match_start_in_q, match_end_in_q, match_pos_in_s);
            it = range_buffer.erase(it);
        } else {
            ++it;
        }
    }
}

// break the big range into its component ranges that we haven't already closed,
// breaking on sequence breaks
void for_each_fresh_range(const match_t& range,
                          atomicbitvector::atomic_bv_t& seen_bv,
                          const seqindex_t& seqidx,
                          const std::function<void(match_t)>& lambda) {
    // walk range, breaking where we've seen it, emiting new ranges
    uint64_t p = range.start;
    pos_t t = range.pos;
    //std::cerr << "for_each_fresh_range " << range.start << "-" << range.end << " " << pos_to_string(range.pos) << std::endl;
    while (p < range.end) {
        // if we haven't seen p, start making a range
        //std::cerr << "looking at " << p << std::endl;
        if (seen_bv.test(p)) { // || seqidx.seq_start(p)) {
            ++p;
            incr_pos(t);
        } else {
            // otherwise, skip along
            uint64_t q = p;
            pos_t v = t;
            while (p < range.end && !seen_bv.test(p)) {
                ++p;
                incr_pos(t);
                //if (seqidx.seq_start(p)) break; // works, but is this the right place?
            }
            //std::cerr << "lambda\t" << q << " " << p << " " << pos_to_string(v) << std::endl;
            lambda({q, p, v});
        }
    }
}

void handle_range(match_t s,
                  atomicbitvector::atomic_bv_t& seen_bv,
                  atomicbitvector::atomic_bv_t& curr_bv,
                  const seqindex_t& seqidx,
                  const uint64_t& query_start,
                  const uint64_t& query_end,
                  std::vector<std::pair<match_t, bool>>& ovlp,
                  range_atomic_queue_t& todo,
                  std::vector<std::pair<pos_t, uint64_t>>& overflow) {
    /*
    std::cerr << "handle_range "
              << s.start << "-" << s.end << " "
              << pos_to_string(s.pos) << " "
              << query_start << " " << query_end << " "
              << std::endl;
    */
    if (s.start < query_end && s.end > query_start) {
//#pragma omp critical (cerr)
        //std::cerr << "seen_range\t" << s.start << "\t" << s.end << "\t" << pos_to_string(s.pos) << std::endl;
        if (query_start > s.start) {
            uint64_t trim_from_start = query_start - s.start;
            s.start += trim_from_start;
            incr_pos(s.pos, trim_from_start);
        }
        if (s.end > query_end) {
            uint64_t trim_from_end = s.end - query_end;
            s.end -= trim_from_end;
        }
        assert(s.start < s.end);
        // record the adjusted range
//#pragma omp critical (ovlp)
        // check if we haven't closed the target range before adding to todo
        bool all_set_there = true;
        pos_t n = s.pos;
        for (uint64_t i = s.start; i < s.end; ++i) {
            assert(!seen_bv[i]);
            all_set_there = curr_bv.set(offset(n)) && all_set_there;
            incr_pos(n);
        }
        ovlp.push_back(std::make_pair(s, is_rev(s.pos)));
        //std::cerr << "all_set ? " << all_set << std::endl;
        if (!all_set_there) {
            /*
            std::cerr << "todo_insert "
                      << pos_to_string(make_pos_t(offset(s.pos),is_rev(s.pos))) << " "
                      << s.end - s.start << std::endl;
            */
            auto item = std::make_pair(make_pos_t(offset(s.pos),is_rev(s.pos)), s.end - s.start);
            // check if we can insert in todo, and insert if we can
            if (!todo.try_push(item)) {
                // if not, put into a thread-local overflow
                overflow.push_back(item); // TODO check if this ever occurs and toss a warning?
            }
        }
    }
}

void explore_overlaps(const match_t& b,
                      atomicbitvector::atomic_bv_t& seen_bv,
                      atomicbitvector::atomic_bv_t& curr_bv,
                      const seqindex_t& seqidx,
                      mmmulti::iitree<uint64_t, pos_t>& aln_iitree,
                      std::vector<std::pair<match_t, bool>>& ovlp,
                      range_atomic_queue_t& todo,
                      std::vector<std::pair<pos_t, uint64_t>>& overflow) {
    std::vector<size_t> o;
    aln_iitree.overlap(b.start, b.end, o);
    for (auto& idx : o) {
        auto r = get_match(aln_iitree, idx);
        for_each_fresh_range(
            r,
            seen_bv,
            seqidx,
            [&](match_t s) {
                handle_range(s, seen_bv, curr_bv, seqidx, b.start, b.end, ovlp, todo, overflow);
            });
    }
}

size_t compute_transitive_closures(
    const seqindex_t& seqidx,
    mmmulti::iitree<uint64_t, pos_t>& aln_iitree, // input alignment matches between query seqs
    const std::string& seq_v_file,
    mmmulti::iitree<uint64_t, pos_t>& node_iitree, // maps graph seq ranges to input seq ranges
    mmmulti::iitree<uint64_t, pos_t>& path_iitree, // maps input seq ranges to graph seq ranges
    uint64_t repeat_max,
    uint64_t transclose_batch_size) { // size of a batch to collect for lock-free transitive closure
    // get our thread count as set for openmp (nb: we'll only partly use openmp here)
    uint nthreads = get_thread_count();
    // open seq_v_file
    std::ofstream seq_v_out(seq_v_file.c_str());
    // remember the elements of Q we've seen
    //std::cerr << "seq_size " << seqidx.seq_length() << std::endl;
    //sdsl::bit_vector q_seen_bv(seqidx.seq_length());
    atomicbitvector::atomic_bv_t q_seen_bv(seqidx.seq_length());
    uint64_t input_seq_length = seqidx.seq_length();
    // a buffer of ranges to write into our iitree, arranged by range ending position in Q
    // we flush those intervals that don't get extended into the next position in S
    // this maps from a position in Q (our input seqs concatenated, offset and orientation)
    // to a range (start and length) in S (our graph sequence vector)
    // we are mapping from the /last/ position in the matched range, not the first
    std::map<pos_t, std::pair<uint64_t, uint64_t>> range_buffer;
    uint64_t last_seq_id = seqidx.seq_id_at(0);
    // collect based on a seed chunk of a given length
    for (uint64_t i = 0; i < input_seq_length; ) {
        // scan our q_seen_bv to find our next start
        //std::cerr << "closing\t" << i << std::endl;
        while (i < input_seq_length && q_seen_bv[i]) ++i;
        //std::cerr << "scanned_to\t" << i << std::endl;
        if (i >= input_seq_length) break; // we're done!
        // where our chunk begins
        uint64_t chunk_start = i;
        // extend until we've got chunk_size unseen bases
        uint64_t bases_to_consider = 0;
        uint64_t chunk_end = chunk_start;
        while (bases_to_consider < transclose_batch_size && chunk_end < input_seq_length) {
            bases_to_consider += !q_seen_bv[chunk_end++];
        }
        // and where it ends (not past the end of the sequence)
        //chunk_end = std::min(input_seq_length, chunk_end); // chunk_start + transclose_batch_size);
        // collect ranges overlapping, per thread to avoid contention
        std::vector<std::vector<std::pair<match_t, bool>>> ovlps(nthreads);
        // bits of sequence we've seen during this union-find chunk
        atomicbitvector::atomic_bv_t q_curr_bv(seqidx.seq_length());
        // a shared work queue for our threads
        range_atomic_queue_t todo; // 16M elements
        //std::cerr << "chunk\t" << chunk_start << "\t" << chunk_end << std::endl;
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "overlap_collect" << std::endl;
        std::atomic<bool> work_todo;
        std::vector<std::atomic<bool>> explorings(nthreads);
        work_todo.store(true);
        auto worker_lambda =
            [&](uint64_t tid) {
                auto& ovlp = ovlps[tid];
                auto& exploring = explorings[tid];
                std::vector<std::pair<pos_t, uint64_t>> overflow;
                // spin while waiting to get our first range
                //std::cerr << "about to spin in thread " << tid << std::endl;
                std::pair<pos_t, uint64_t> item;
                //while (work_todo.load() && !todo.try_pop(item));
                //if (!work_todo.load()) return;
                // then continue until the work queue is apparently empty
                while (work_todo.load()) {
                    if (todo.try_pop(item)) {
                        exploring.store(true);
                        auto& pos = item.first;
                        auto& match_len = item.second;
                        uint64_t n = !is_rev(pos) ? offset(pos) : offset(pos) - match_len + 1;
                        uint64_t range_start = n;
                        uint64_t range_end = n + match_len;
                        explore_overlaps({range_start, range_end, pos},
                                         q_seen_bv,
                                         q_curr_bv,
                                         seqidx,
                                         aln_iitree,
                                         ovlp,
                                         todo,
                                         overflow);
                        // try to flush items from our thread local overflow into todo
                        //std::cerr << "thread " << tid << " overflow.size() " << overflow.size() << std::endl;
                    }
                    while (overflow.size() && todo.try_push(overflow.back())) {
                        overflow.pop_back();
                    }
                    exploring.store(false);
                }
            };
        // launch our threads to expand the overlap set in parallel
        std::vector<std::thread> workers; workers.reserve(nthreads);
        for (uint64_t t = 0; t < nthreads; ++t) {
            workers.emplace_back(worker_lambda, t);
        }
        // the chunk range isn't an actual alignment, so we handle it differently
        for_each_fresh_range({chunk_start, chunk_end, 0}, q_seen_bv, seqidx, [&](match_t b) {
                // the special case is handling ranges that have no matches
                // we need to close these even if they aren't matched to anything
                //std::cerr << "upfront\t" << b.start << "-" << b.end << std::endl;
                for (uint64_t j = b.start; j < b.end; ++j) {
                    //q_subset.push_back(j); //make_pos_t(j, false));
                    //q_subset.push_back(make_pos_t(j, true));
                    assert(!q_seen_bv[j]);
                    q_curr_bv.set(j);
                }
                //std::cerr << "outer_lookup " << b.start << " " << b.end << std::endl;
                todo.push(std::make_pair(make_pos_t(b.start, false), b.end - b.start));
                // TODO use this threading model for the recursive exploration
                //std::thread t([&](void) { explore_overlaps(b, q_seen_bv, q_curr_bv, aln_iitree, ovlp, todo); });
                //t.join();
            });
        // avoid a potential race that occurs when we don't have enough work for all the threads
        std::this_thread::sleep_for(1ms);
        uint64_t empty_iter_count = 0;
        auto still_exploring
            = [&explorings](void) {
                  bool ongoing = false;
                  for (auto& e : explorings) {
                      ongoing = ongoing || e.load();
                  }
                  return ongoing;
              };
        while (!todo.was_empty() || still_exploring()) {// || empty_iter_count < 10) {
            std::this_thread::sleep_for(1ms);
            //++empty_iter_count;
        }
        work_todo.store(false);
        //std::cerr << "gonna join" << std::endl;
        for (uint64_t t = 0; t < nthreads; ++t) {
            workers[t].join();
        }
        // TODO use a thread to collect these during runtime from another atomic ring buffer
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "overlaps_vector_merge" << std::endl;
        uint64_t novlps = 0;
        for (auto& v : ovlps) novlps += v.size();
        std::vector<std::pair<match_t, bool>> ovlp;
        ovlp.reserve(novlps);
        for (auto& v : ovlps) {
            ovlp.insert(ovlp.end(), v.begin(), v.end());
            v.clear(); // attempt to free memory
        }
        // print our overlaps
        /*
        std::cerr << "transc" << "\t" << chunk_start << "-" << chunk_end << std::endl;
        for (auto& s : ovlp) {
            std::cerr << "ovlp" << "\t" << s.first.start << "-" << s.first.end << "\t" << offset(s.first.pos) << (is_rev(s.first.pos)?"-":"+") << "\t" << (s.second?"-":"+") << std::endl;
        }
        */
        // run the transclosure for this region using lock-free union find
        // convert the ranges into positions in the input sequence space
        uint64_t q_curr_bv_count = 0;
        //std::cerr << "q_subset_bv ";
        for (auto x : q_curr_bv) {
            ++q_curr_bv_count;
        }
        // use a rank support to make a dense mapping from the current bases to an integer range
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "rank_build" << std::endl;
        std::vector<uint64_t> q_curr_bv_vec; q_curr_bv_vec.reserve(q_curr_bv_count);
        for (auto p : q_curr_bv) {
            q_curr_bv_vec.push_back(p);
        }
        sdsl::bit_vector q_curr_bv_sdsl(seqidx.seq_length());
        for (auto p : q_curr_bv_vec) {
            q_curr_bv_sdsl[p] = 1;
        }
        sdsl::bit_vector::rank_1_type q_curr_rank;
        sdsl::util::assign(q_curr_rank, sdsl::bit_vector::rank_1_type(&q_curr_bv_sdsl));
        //q_curr_bv_vec.clear();
        // disjoint set structure
        std::vector<DisjointSets::Aint> q_sets_data(q_curr_bv_count);
        // this initializes everything
        auto disjoint_sets = DisjointSets(q_sets_data.data(), q_sets_data.size());
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "parallel_union_find" << std::endl;
#pragma omp parallel for
        for (uint64_t k = 0; k < ovlp.size(); ++k) {
            auto& s = ovlp.at(k);
            auto& r = s.first;
            pos_t p = r.pos;
            for (uint64_t j = r.start; j != r.end; ++j) {
                // unite both sides of the overlap
                disjoint_sets.unite(q_curr_rank(j), q_curr_rank(offset(p)));
                incr_pos(p);
            }
        }
        // now read out our transclosures
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "dset_write" << std::endl;
        std::vector<std::pair<uint64_t, uint64_t>> dsets(q_curr_bv_count);
        std::pair<uint64_t, uint64_t> max_pair = std::make_pair(std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max());
#pragma omp parallel for
        for (uint64_t j = 0; j < q_curr_bv_count; ++j) {
            auto& p = q_curr_bv_vec[j];
            if (!q_seen_bv[p]) {
                dsets[j] = std::make_pair(disjoint_sets.find(q_curr_rank(p)), p);
            } else {
                dsets[j] = max_pair;
            }
        }
        //q_curr_bv_vec.clear();
        // remove excluded elements
        dsets.erase(std::remove_if(dsets.begin(), dsets.end(),
                                   [&max_pair](const std::pair<uint64_t, uint64_t>& x) {
                                       return x == max_pair;
                                   }),
                    dsets.end());
        // compress the dsets
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "dset_compression" << std::endl;
        ips4o::parallel::sort(dsets.begin(), dsets.end());

        uint64_t c = 0;
        assert(dsets.size());
        uint64_t l = dsets.front().first;
        for (auto& d : dsets) {
            if (d.first != l) {
                ++c;
                l = d.first;
            }
            d.first = c;
        }
        /*
        for (auto& d : dsets) {
            std::cerr << "sdset\t" << d.first << "\t" << d.second << std::endl;
        }
        */
        // sort by the smallest starting position in each disjoint set
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "dset_sort" << std::endl;
        std::vector<std::pair<uint64_t, uint64_t>> dsets_by_min_pos(c+1);
        for (uint64_t x = 0; x < c+1; ++x) {
            dsets_by_min_pos[x].second = x;
            dsets_by_min_pos[x].first = std::numeric_limits<uint64_t>::max();
        }
        for (auto& d : dsets) {
            uint64_t& minpos = dsets_by_min_pos[d.first].first;
            minpos = std::min(minpos, d.second);
        }
        ips4o::parallel::sort(dsets_by_min_pos.begin(), dsets_by_min_pos.end());
        /*
        for (auto& d : dsets_by_min_pos) {
            std::cerr << "sdset_min_pos\t" << d.second << "\t" << d.first << std::endl;
        }
        */
        // invert the naming
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "dset_invert" << std::endl;
        std::vector<uint64_t> dset_names(c+1);
        uint64_t x = 0;
        for (auto& d : dsets_by_min_pos) {
            dset_names[d.second] = x++;
        }
        // rename sdsets and re-sort
        for (auto& d : dsets) {
            d.first = dset_names[d.first];
        }
        ips4o::parallel::sort(dsets.begin(), dsets.end());
        /*
        for (auto& d : dsets) {
            std::cerr << "sdset_rename\t" << d.first << "\t" << pos_to_string(d.second) << std::endl;
        }
        */
        // now, run the graph emission
        //std::cerr << "transclosure" << "\t" << chunk_start << "-" << chunk_end << "\t" << "graph_emission" << std::endl;
        size_t seq_v_length = seq_v_out.tellp();
        //uint64_t flushed = range_buffer.size();
        uint64_t last_dset_id = std::numeric_limits<uint64_t>::max(); // ~inf
        char current_base = '\0';
        // determine if we've switched references
        for (auto& d : dsets) {
            const auto& curr_dset_id = d.first;
            const auto& curr_offset = d.second;
            char base = seqidx.at(curr_offset);
            // if we're on a new position
            if (curr_dset_id != last_dset_id) {
                // emit our new position
                current_base = base;
                seq_v_out << base;
                ++seq_v_length;
                // check to see if we've switched sequences
                // this check assumes that we're walking up through the Q vector
                // we take the minimum position in Q in the dset and ask if it implies a sequence switch
                uint64_t curr_seq_id = seqidx.seq_id_at(curr_offset);
                // if we've changed basis sequences, flush
                if (curr_seq_id != last_seq_id) {
                    flush_ranges(seq_v_length, range_buffer, node_iitree, path_iitree); // hack to force flush at sequence change
                    last_seq_id = curr_seq_id;
                } else {
                    flush_ranges(seq_v_length-1, range_buffer, node_iitree, path_iitree);
                }
                last_dset_id = curr_dset_id;
            }
            pos_t curr_q_pos = make_pos_t(curr_offset, false);
            if (current_base != seqidx.at_pos(curr_q_pos)) {
                curr_q_pos = make_pos_t(curr_offset, true);
            }
            assert(current_base = seqidx.at_pos(curr_q_pos));
            extend_range(seq_v_length-1, curr_q_pos, range_buffer);
            //for (auto curr_q_pos : {make_pos_t(d.second, false), make_pos_t(d.second, true) })
            //char base = seqidx.at(offset(curr_q_pos));
            // filter one strand
            //if (current_base == seqidx.at_pos(curr_q_pos)) {
                // in any case, use extend_range
            //extend_range(seq_v_length-1, curr_q_pos, range_buffer);
            //}
            // dump the range buffer
	    /*
            std::cerr << "============================================================" << std::endl;
            std::cerr << "dset_pos " << seq_v_length << std::endl;
            for (auto& r : range_buffer) {
                std::cerr << "range_buffer " << pos_to_string(r.first) << " " << r.second.first << " " << r.second.second << std::endl;
            }
	    */
        }
        // mark our q_seen_bv for later
        for (auto p : q_curr_bv_vec) {
            //std::cerr << "marking_q_seen_bv " << offset(p) << std::endl;
            q_seen_bv.set(p); // mark that we're closing over these bases
        }
        /*
        std::cerr << "q_curr_bv\t";
        for (uint64_t j = 0; j < q_curr_bv.size(); ++j) {
            std::cerr << q_curr_bv[j];
        }
        std::cerr << std::endl;
        std::cerr << "q_seen_bv\t";
        for (uint64_t j = 0; j < q_seen_bv.size(); ++j) {
            std::cerr << q_seen_bv[j];
        }
        std::cerr << std::endl;
        */
        //flush_ranges(seq_v_length);
        i = chunk_end; // update our chunk end here!
    }
    //exit(1);
    // close the graph sequence vector
    size_t seq_bytes = seq_v_out.tellp();
    seq_v_out.close();
    flush_ranges(seq_bytes+1, range_buffer, node_iitree, path_iitree);
    assert(range_buffer.empty());
    // build node_mm and path_mm indexes
    node_iitree.index();
    path_iitree.index();
    return seq_bytes;
}

}
