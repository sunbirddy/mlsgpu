/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Extract disk statistics from the operating system
 */

#ifndef DISKSTATS_H
#define DISKSTATS_H

#include <vector>
#include <string>

namespace Diskstats
{

struct Snapshot
{
    long long bytesRead;
    long long bytesWritten;
    long long readRequests;
    long long writeRequests;
};

void initialize(const std::vector<std::string> &disknames);

Snapshot snapshot();

Snapshot operator -(const Snapshot &a, const Snapshot &b);

void saveStatistics(const Snapshot &snap, const std::string &prefix);

} // namespace Diskstats

#endif /* DISKSTATS_H */