//
// emuscirclecompat.h
//
// Compatibility shim for building libEmuSC under Circle.
//
// mt32-pi's framework
// Version: 2026-10-21
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header is force-included (-include) when compiling libEmuSC for
// Circle (see the "emusc" target in the Makefile). It is not used by the
// mt32-pi kernel sources themselves.
//
// The aarch64-none-elf libstdc++ used by mt32-pi is built without gthread
// support, so <mutex> provides no std::mutex (its std::scoped_lock RAII
// wrapper is still defined), and libEmuSC cannot compile without one.
//
// That protection is not needed in the mt32-pi integration, and this is
// what makes the no-op replacements below sound:
// - libEmuSC never spawns threads of its own;
// - CEmuSCSynth serializes every libEmuSC entry point (MIDI input, SysEx,
//   rendering, reset, panic, volume) with its CSynthBase spinlock, exactly
//   like the other synth engines, so the MIDI/render race the mutexes
//   guard against cannot occur.
//
#ifndef _mt32pi_emuscirclecompat_h
#define _mt32pi_emuscirclecompat_h

namespace std
{
	class mutex
	{
	public:
		constexpr mutex() noexcept = default;
		mutex(const mutex&) = delete;
		mutex& operator=(const mutex&) = delete;

		void lock() noexcept { }
		bool try_lock() noexcept { return true; }
		void unlock() noexcept { }
	};

}

#endif
