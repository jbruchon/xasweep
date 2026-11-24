# xasweep

**eXcess Allocation Sweep** cuts the speculative pre-allocation tails off of files to free up storage space, primarily on XFS filesystems under certain access patterns. Official XFS documentation states that these excess allocations (meant to reduce or avoid fragmentation) are removed once the file being written in closed, but real-world behavior doesn't match the docs. I have personally freed multiple GB of storage space in some cases using a hacked-together Bash script that `truncate`s the files to their stated sizes; this is basically a C version of that Bash script, and should be a lot faster.

If you find this software useful, please consider donating to support its continued development! Visit [jodybruchon.com](https://www.jodybruchon.com/) for donation links and more of my projects.

## Building

Build with `make`. It's really that easy!

## License

Copyright (C) 2026 by Jody Lee Bruchon `<jody@jodybruchon.com>`. Distributed under the GNU General Public License v2.
