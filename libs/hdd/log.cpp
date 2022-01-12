/***************************************************************************
 *   Copyright (C) by ETHZ/SED                                             *
 *                                                                         *
 * This program is free software: you can redistribute it and/or modify    *
 * it under the terms of the GNU LESSER GENERAL PUBLIC LICENSE as          *
 * published by the Free Software Foundation, either version 3 of the      *
 * License, or (at your option) any later version.                         *
 *                                                                         *
 * This software is distributed in the hope that it will be useful,        *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU Affero General Public License for more details.                     *
 *                                                                         *
 *   Developed by Luca Scarabello <luca.scarabello@sed.ethz.ch>            *
 ***************************************************************************/

#include "log.h"

using namespace std;

namespace HDD {

std::function<void(const std::string &)> Logger::_error;
std::function<void(const std::string &)> Logger::_warning;
std::function<void(const std::string &)> Logger::_info;
std::function<void(const std::string &)> Logger::_debug;
std::function<void *(const std::string &, const std::vector<Logger::Level> &)>
    Logger::_createFileLogger;
std::function<void(void *)> Logger::_destroyFileLogger;

} // namespace HDD
