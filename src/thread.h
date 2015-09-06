/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef THREAD_H_INCLUDED
#define THREAD_H_INCLUDED

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "material.h"
#include "movepick.h"
#include "pawns.h"
#include "position.h"
#include "search.h"
#include "thread_win32.h"

struct Thread;

const size_t MAX_THREADS = 128;
const size_t MAX_SPLITPOINTS_PER_THREAD = 8;
const int    MAX_SLAVES_PER_SPLITPOINT = 7;

class Spinlock {

  std::atomic_int lock;

public:
  Spinlock() { lock = 1; } // Init here to workaround a bug with MSVC 2013
  void acquire() {
      while (lock.fetch_sub(1, std::memory_order_acquire) != 1)
          while (lock.load(std::memory_order_relaxed) <= 0)
              std::this_thread::yield(); // Be nice to hyperthreading
  }
  void release() { lock.store(1, std::memory_order_release); }
};


/// SplitPoint struct stores information shared by the threads searching in
/// parallel below the same split point. It is populated at splitting time.

struct SplitPoint {

  // Const data after split point has been setup
  const Position* pos;
  Search::Stack* ss;
  Thread* master;
  Depth depth;
  Value beta;
  int nodeType;
  bool cutNode;

  // Const pointers to shared data
  MovePicker* movePicker;
  SplitPoint* parentSplitPoint;

  // Shared variable data
  Spinlock spinlock;
  std::bitset<MAX_THREADS> slavesMask;
  volatile bool allSlavesSearching;
  volatile uint64_t nodes;
  volatile Value alpha;
  volatile Value bestValue;
  volatile Move bestMove;
  volatile int moveCount;
  volatile bool cutoff;
};


/// ThreadBase struct is the base of the hierarchy from where we derive all the
/// specialized thread classes.

struct ThreadBase : public std::thread {

  virtual ~ThreadBase() = default;
  virtual void idle_loop() = 0;
  void notify_one();
  void wait_for(volatile const bool& b);

  Mutex mutex;
  Spinlock spinlock;
  ConditionVariable sleepCondition;
  volatile bool exit = false;
};


/// Thread struct keeps together all the thread related stuff like locks, state
/// and especially split points. We also use per-thread pawn and material hash
/// tables so that once we get a pointer to an entry its life time is unlimited
/// and we don't have to care about someone changing the entry under our feet.

struct Thread : public ThreadBase {

  Thread();
  virtual void idle_loop();
  bool cutoff_occurred() const;
  bool can_join(const SplitPoint* sp) const;

  void split(Position& pos, Search::Stack* ss, Value alpha, Value beta, Value* bestValue, Move* bestMove,
             Depth depth, int moveCount, MovePicker* movePicker, int nodeType, bool cutNode);

  SplitPoint splitPoints[MAX_SPLITPOINTS_PER_THREAD];
  Pawns::Table pawnsTable;
  Material::Table materialTable;
  Endgames endgames;
  Position* activePosition;
  size_t idx;
  int maxPly;

  // Secure access (RW) for the following variables only when the thread specific spinlock is locked.
  // Speculative/delayed access (RO) also without a lock.
  SplitPoint* volatile activeSplitPoint;
  volatile size_t splitPointsSize;
  volatile bool searching;
};


/// MainThread and TimerThread are derived classes used to characterize the two
/// special threads: the main one and the recurring timer.

struct MainThread : public Thread {
  virtual void idle_loop();
  void join();
  volatile bool thinking = true; // Avoid a race with start_thinking()
};

struct TimerThread : public ThreadBase {

  static const int Resolution = 5; // Millisec between two check_time() calls

  virtual void idle_loop();

  bool run = false;
};


/// ThreadPool struct handles all the threads related stuff like init, starting,
/// parking and, most importantly, launching a slave thread at a split point.
/// All the access to shared thread data is done through this class.

struct ThreadPool : public std::vector<Thread*> {

  void init(); // No c'tor and d'tor, threads rely on globals that should be
  void exit(); // initialized and are valid during the whole thread lifetime.

  MainThread* main() { return static_cast<MainThread*>(at(0)); }
  void read_uci_options();
  Thread* available_slave(const SplitPoint* sp) const;
  void start_thinking(const Position&, const Search::LimitsType&, Search::StateStackPtr&);
  size_t  max_slaves_per_splitpoint(Depth depth);
  Depth minimumSplitDepth;
  TimerThread* timer;
};

extern ThreadPool Threads;

#endif // #ifndef THREAD_H_INCLUDED
