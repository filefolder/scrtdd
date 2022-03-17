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

#include "dd.h"
#include "log.h"
#include "random.h"
#include "utils.h"
#include "xcorr.h"

using namespace std;
using std::chrono::duration;
using Event     = HDD::Catalog::Event;
using Phase     = HDD::Catalog::Phase;
using Station   = HDD::Catalog::Station;
using Transform = HDD::Waveform::Processor::Transform;
using HDD::Waveform::getBandAndInstrumentCodes;

namespace {

struct WfCounters
{
  unsigned downloaded;
  unsigned no_avail;
  unsigned disk_cached;
  unsigned snr_low;

  void update(shared_ptr<HDD::Waveform::Loader> &loader)
  {
    HDD::Waveform::BasicLoader *basic =
        dynamic_cast<HDD::Waveform::BasicLoader *>(loader.get());
    if (basic)
    {
      downloaded += basic->_counters_wf_downloaded;
      basic->_counters_wf_downloaded = 0;
      no_avail += basic->_counters_wf_no_avail;
      basic->_counters_wf_no_avail = 0;
      return;
    }

    HDD::Waveform::BatchLoader *batch =
        dynamic_cast<HDD::Waveform::BatchLoader *>(loader.get());
    if (batch)
    {
      downloaded += batch->_counters_wf_downloaded;
      batch->_counters_wf_downloaded = 0;
      no_avail += batch->_counters_wf_no_avail;
      batch->_counters_wf_no_avail = 0;
      return;
    }
  }

  void update(shared_ptr<HDD::Waveform::BatchLoader> &loader)
  {
    if (loader)
    {
      downloaded += loader->_counters_wf_downloaded;
      loader->_counters_wf_downloaded = 0;
      no_avail += loader->_counters_wf_no_avail;
      loader->_counters_wf_no_avail = 0;
    }
  }

  void update(shared_ptr<HDD::Waveform::DiskCachedLoader> &diskCache)
  {
    if (diskCache)
    {
      disk_cached += diskCache->_counters_wf_cached;
      diskCache->_counters_wf_cached = 0;
    }
  }

  void update(shared_ptr<HDD::Waveform::SnrFilterPrc> &snrFilter)
  {
    if (snrFilter)
    {
      snr_low += snrFilter->_counters_wf_snr_low;
      snrFilter->_counters_wf_snr_low = 0;
    }
  }

  void update(shared_ptr<HDD::Waveform::Loader> &loader,
              shared_ptr<HDD::Waveform::DiskCachedLoader> &diskCache,
              shared_ptr<HDD::Waveform::SnrFilterPrc> &snrFilter)
  {
    update(loader);
    update(diskCache);
    update(snrFilter);
  }
};

} // namespace

namespace HDD {

DD::DD(const Catalog &catalog,
       const Config &cfg,
       const string &workingDir,
       std::unique_ptr<HDD::TravelTimeTable> ttt)
    : _cfg(cfg), _srcCat(catalog),
      _bgCat(*Catalog::filterPhasesAndSetWeights(_srcCat,
                                                 Phase::Source::CATALOG,
                                                 _cfg.validPphases,
                                                 _cfg.validSphases)),
      _ttt(std::move(ttt)), _workingDir(workingDir),
      _cacheDir(joinPath(_workingDir, "wfcache")),
      _tmpCacheDir(joinPath(_workingDir, "tmpcache"))
{
  if (!pathExists(_workingDir))
  {
    if (!createDirectories(_workingDir))
    {
      string msg = "Unable to create working directory: " + _workingDir;
      throw Exception(msg);
    }
  }

  if (!pathExists(_cacheDir))
  {
    if (!createDirectories(_cacheDir))
    {
      string msg = "Unable to create cache directory: " + _cacheDir;
      throw Exception(msg);
    }
  }

  if (!pathExists(_tmpCacheDir))
  {
    if (!createDirectories(_tmpCacheDir))
    {
      string msg = "Unable to create cache directory: " + _tmpCacheDir;
      throw Exception(msg);
    }
  }

  setUseCatalogWaveformDiskCache(true);
  setWaveformCacheAll(false);
}

void DD::setUseCatalogWaveformDiskCache(bool cache)
{
  _useCatalogWaveformDiskCache = cache;
  createWaveformCache();
}

void DD::createWaveformCache()
{
  _wfAccess.loader.reset(new Waveform::BasicLoader(_cfg.recordStreamURL));
  _wfAccess.diskCache = nullptr;
  _wfAccess.extraLen  = nullptr;
  _wfAccess.snrFilter = nullptr;
  _wfAccess.memCache  = nullptr;

  shared_ptr<Waveform::Loader> currLdr = _wfAccess.loader;

  if (_useCatalogWaveformDiskCache)
  {
    _wfAccess.diskCache.reset(
        new Waveform::DiskCachedLoader(currLdr, _cacheDir));
    _wfAccess.extraLen.reset(
        new Waveform::ExtraLenLoader(_wfAccess.diskCache, DISK_TRACE_MIN_LEN));
    currLdr = _wfAccess.extraLen;
  }

  shared_ptr<Waveform::Processor> currProc(
      new Waveform::BasicProcessor(currLdr));

  if (_cfg.snr.minSnr > 0)
  {
    _wfAccess.snrFilter.reset(new Waveform::SnrFilterPrc(
        currProc, _cfg.snr.minSnr, _cfg.snr.noiseStart, _cfg.snr.noiseEnd,
        _cfg.snr.signalStart, _cfg.snr.signalEnd));
    currProc = _wfAccess.snrFilter;
  }

  _wfAccess.memCache.reset(new Waveform::MemCachedProc(currProc));
}

void DD::replaceWaveformCacheLoader(const shared_ptr<Waveform::Loader> &baseLdr)
{
  if (_useCatalogWaveformDiskCache)
  {
    _wfAccess.diskCache->setAuxLoader(baseLdr);
  }
  else if (_cfg.snr.minSnr > 0)
  {
    _wfAccess.snrFilter->setAuxProcessor(
        shared_ptr<Waveform::Processor>(new Waveform::BasicProcessor(baseLdr)));
  }
  else
  {
    _wfAccess.memCache->setAuxProcessor(
        shared_ptr<Waveform::Processor>(new Waveform::BasicProcessor(baseLdr)));
  }
}

string DD::generateWorkingSubDir(const string &prefix) const
{
  UTCTime now = UTCClock::now();
  int year, month, day, hour, min, sec, usec;
  UTCClock::toDate(now, year, month, day, hour, min, sec, usec);

  UniformRandomer ran(0, 9999);
  ran.setSeed(durToSec(now.time_since_epoch()));

  // preifx_creationTime_randomNumber
  string id = strf("%s_%04d%02d%02d%02d%02d%02d_%04zu", prefix.c_str(), year,
                   month, day, hour, min, sec, ran.next());
  return id;
}

string DD::generateWorkingSubDir(const Event &ev) const
{
  int year, month, day, hour, min, sec, usec;
  UTCClock::toDate(ev.time, year, month, day, hour, min, sec, usec);
  // singleevent_eventTime_latitude_longitude
  string prefix =
      strf("singleevent_%04d%02d%02d%02d%02d%02d_%05d_%06d", year, month, day,
           hour, min, sec, int(ev.latitude * 1000), int(ev.longitude * 1000));
  return generateWorkingSubDir(prefix);
}

void DD::preloadWaveforms()
{
  //
  // preload waveforms, store them on disk and cache them in memory
  // (already processed).
  // For better performance we want to load waveforms in batch
  // (batchloader)
  //
  logInfo("Loading catalog waveform data (%lu events to load)",
          _bgCat.getEvents().size());

  auto forEventWaveforms =
      [this](const Event &event, unsigned &numPhases, unsigned &numSPhases,
             std::function<void(const TimeWindow &, const Event &,
                                const Phase &, const string &)> func) {
        auto eqlrng = _bgCat.getPhases().equal_range(event.id);
        for (auto it = eqlrng.first; it != eqlrng.second; ++it)
        {
          const Phase &phase  = it->second;
          TimeWindow tw       = xcorrTimeWindowLong(phase);
          const auto xcorrCfg = _cfg.xcorr.at(phase.procInfo.type);

          for (const string &component : xcorrCfg.components)
          {
            func(tw, event, phase, component);
          }

          numPhases++;
          if (phase.procInfo.type == Phase::Type::S) numSPhases++;
        }
      };

  unsigned numPhases = 0, numSPhases = 0, numEvents = 0;
  WfCounters wfcount{0};

  for (const auto &kv : _bgCat.getEvents())
  {
    const Event &event = kv.second;

    logDebug("Loading event %u waveforms...", event.id);

    shared_ptr<Waveform::BatchLoader> batchLoader(
        new Waveform::BatchLoader(_cfg.recordStreamURL));
    replaceWaveformCacheLoader(batchLoader);

    // the following doesn't load anything unless the waveform is not alreday on
    // the disk cache, instead it records in BatchLoader the waveforms we want
    // to download
    auto requestWaveform = [this](const TimeWindow &tw, const Event &ev,
                                  const Phase &ph, const string &component) {
      getWaveform(*_wfAccess.memCache, tw, ev, ph, component);
    };

    forEventWaveforms(event, numPhases, numSPhases, requestWaveform);

    // This will actually dowanload the waveworms
    batchLoader->load();

    // now process and store the waveforms into memory
    auto cacheWaveform = [this](const TimeWindow &tw, const Event &ev,
                                const Phase &ph, const string &component) {
      getWaveform(*_wfAccess.memCache, tw, ev, ph, component);
    };

    unsigned discard;
    forEventWaveforms(event, discard, discard, cacheWaveform);

    wfcount.update(batchLoader);

    const unsigned onePercent =
        _bgCat.getEvents().size() < 100 ? 1 : (_bgCat.getEvents().size() / 100);
    if (++numEvents % onePercent == 0)
    {
      logInfo("Loaded %.1f%% of catalog phase waveforms",
              (numEvents * 100.0 / _bgCat.getEvents().size()));
    }
  }

  // restore proper loader
  replaceWaveformCacheLoader(shared_ptr<Waveform::Loader>(
      new Waveform::BasicLoader(_cfg.recordStreamURL)));

  wfcount.update(_wfAccess.loader, _wfAccess.diskCache, _wfAccess.snrFilter);

  logInfo("Finished preloading catalog waveform data: total events %lu total "
          "phases %u (P %.f%%, S %.f%%). Waveforms downloaded %u, not "
          "available %u, loaded from disk cache %u",
          _bgCat.getEvents().size(), numPhases,
          ((numPhases - numSPhases) * 100. / numPhases),
          (numSPhases * 100. / numPhases), wfcount.downloaded, wfcount.no_avail,
          wfcount.disk_cached);
}

void DD::dumpWaveforms(const string &basePath)
{
  if (!basePath.empty() && !pathExists(basePath))
  {
    if (!createDirectories(basePath))
    {
      throw Exception("Unable to create waveform dump directory: " + basePath);
    }
  }

  auto waveformPath = [](const string &wfDebugDir, const Catalog::Event &ev,
                         const Catalog::Phase &ph, const string &channelCode,
                         const string &ext) -> string {
    string debugFile =
        HDD::strf("ev%u.%s.%s.%s.%s.%s.%s.mseed", ev.id, ph.networkCode.c_str(),
                  ph.stationCode.c_str(), ph.locationCode.c_str(),
                  channelCode.c_str(), ph.type.c_str(), ext.c_str());
    return HDD::joinPath(wfDebugDir, debugFile);
  };

  for (const auto &kv : _bgCat.getEvents())
  {
    const Event &event = kv.second;
    auto eqlrng        = _bgCat.getPhases().equal_range(event.id);
    for (auto it = eqlrng.first; it != eqlrng.second; ++it)
    {
      const Phase &phase  = it->second;
      TimeWindow tw       = xcorrTimeWindowLong(phase);
      const auto xcorrCfg = _cfg.xcorr.at(phase.procInfo.type);

      for (const string &component : xcorrCfg.components)
      {
        // getAuxProcessor() -> we don't really want to keep the waveforms in
        // memory
        shared_ptr<const Trace> tr =
            getWaveform(*_wfAccess.memCache->getAuxProcessor(), tw, event,
                        phase, component);

        // check low SNR
        bool lowSNR = false;
        if (!tr && _wfAccess.snrFilter)
        {
          _wfAccess.snrFilter->setEnabled(false);
          tr = getWaveform(*_wfAccess.memCache->getAuxProcessor(), tw, event,
                           phase, component);

          if (tr) lowSNR = true;
          _wfAccess.snrFilter->setEnabled(true);
        }

        if (!tr) continue;

        string channelCode =
            getBandAndInstrumentCodes(phase.channelCode) + component;
        string ext =
            (phase.procInfo.source == Catalog::Phase::Source::THEORETICAL)
                ? "xcorrDetected"
                : (phase.isManual ? "manual" : "automatic");
        if (lowSNR) ext += ".lowSNR";

        const string wfPath =
            waveformPath(basePath, event, phase, channelCode, ext);

        logInfo("Writing %s", wfPath.c_str());
        Waveform::writeTrace(*tr, wfPath);
      }
    }
  }
}

void DD::dumpClusters(const ClusteringOptions &clustOpt, const string &basePath)
{
  if (!basePath.empty() && !pathExists(basePath))
  {
    if (!createDirectories(basePath))
    {
      throw Exception("Unable to create clusters dump directory: " + basePath);
    }
  }
  // find Neighbours for each event in the catalog
  unordered_map<unsigned, unique_ptr<Neighbours>> neighboursByEvent =
      selectNeighbouringEventsCatalog(
          _bgCat, clustOpt.minWeight, clustOpt.minESdist, clustOpt.maxESdist,
          clustOpt.minEStoIEratio, clustOpt.minDTperEvt, clustOpt.maxDTperEvt,
          clustOpt.minNumNeigh, clustOpt.maxNumNeigh, clustOpt.numEllipsoids,
          clustOpt.maxEllipsoidSize, true);

  // Organize the neighbours by not connected clusters
  list<unordered_map<unsigned, unique_ptr<Neighbours>>> clusters =
      clusterizeNeighbouringEvents(neighboursByEvent);

  logInfo("Found %lu event clusters", clusters.size());

  unsigned clusterId = 1;
  for (const auto &neighCluster : clusters)
  {
    logInfo("Writing cluster %u (%lu events)", clusterId, neighCluster.size());

    Catalog catToDump;
    for (const auto &kv : neighCluster) catToDump.add(kv.first, _bgCat, true);
    string prefix = strf("cluster-%u", clusterId++);
    catToDump.writeToFile(joinPath(basePath, (prefix + "-event.csv")),
                          joinPath(basePath, (prefix + "-phase.csv")),
                          joinPath(basePath, (prefix + "-station.csv")));
  }
}

unique_ptr<Catalog> DD::relocateMultiEvents(const ClusteringOptions &clustOpt,
                                            const SolverOptions &solverOpt)
{
  logInfo("Starting DD relocator in multiple events mode");

  Catalog catToReloc(_bgCat);

  // prepare a folder for debug files
  string catalogWorkingDir;
  do
  {
    catalogWorkingDir = generateWorkingSubDir("multievent");
    catalogWorkingDir = joinPath(_workingDir, catalogWorkingDir);
  } while (pathExists(catalogWorkingDir));

  if (_saveProcessing)
  {
    if (!pathExists(catalogWorkingDir))
    {
      if (!createDirectories(catalogWorkingDir))
      {
        string msg = "Unable to create working directory: " + catalogWorkingDir;
        throw Exception(msg);
      }
    }
    logInfo("Working dir %s", catalogWorkingDir.c_str());
  }

  // prepare file logger
  unique_ptr<Logger::File> fileLogger;
  if (_saveProcessing)
  {
    string logFile = joinPath(catalogWorkingDir, "info.log");
    fileLogger =
        Logger::toFile(logFile, {Logger::Level::info, Logger::Level::warning,
                                 Logger::Level::error});
  }

  // find Neighbours for each event in the catalog
  unordered_map<unsigned, unique_ptr<Neighbours>> neighboursByEvent =
      selectNeighbouringEventsCatalog(
          catToReloc, clustOpt.minWeight, clustOpt.minESdist,
          clustOpt.maxESdist, clustOpt.minEStoIEratio, clustOpt.minDTperEvt,
          clustOpt.maxDTperEvt, clustOpt.minNumNeigh, clustOpt.maxNumNeigh,
          clustOpt.numEllipsoids, clustOpt.maxEllipsoidSize, true);

  // Organize the neighbours by not connected clusters
  list<unordered_map<unsigned, unique_ptr<Neighbours>>> clusters =
      clusterizeNeighbouringEvents(neighboursByEvent);

  logInfo("Found %lu event clusters", clusters.size());

  if (_saveProcessing)
  {
    catToReloc.writeToFile(joinPath(catalogWorkingDir, "starting-event.csv"),
                           joinPath(catalogWorkingDir, "starting-phase.csv"),
                           joinPath(catalogWorkingDir, "starting-station.csv"));
  }

  //
  // relocate one cluster a time
  //
  unique_ptr<Catalog> relocatedCatalog(new Catalog());

  unsigned clusterId = 1;
  for (const auto &neighCluster : clusters)
  {
    logInfo("Relocating cluster %u (%lu events)", clusterId,
            neighCluster.size());

    if (_saveProcessing)
    {
      Catalog catToDump;
      for (const auto &kv : neighCluster)
        catToDump.add(kv.first, catToReloc, true);
      string prefix = strf("cluster-%u", clusterId);
      catToDump.writeToFile(
          joinPath(catalogWorkingDir, (prefix + "-event.csv")),
          joinPath(catalogWorkingDir, (prefix + "-phase.csv")),
          joinPath(catalogWorkingDir, (prefix + "-station.csv")));
    }

    // Perform cross-correlation which also detects picks around theoretical
    // arrival times. The catalog will be updated with the corresponding
    // theoretical phases.
    const XCorrCache xcorr = buildXCorrCache(
        catToReloc, neighCluster, _useArtificialPhases,
        clustOpt.xcorrMaxEvStaDist, clustOpt.xcorrMaxInterEvDist);

    // the actual relocation
    unique_ptr<Catalog> relocatedCluster =
        relocate(catToReloc, neighCluster, solverOpt, false, xcorr);

    relocatedCatalog->add(*relocatedCluster, true);

    if (_saveProcessing)
    {
      string prefix = strf("relocated-cluster-%u", clusterId);
      relocatedCluster->writeToFile(
          joinPath(catalogWorkingDir, (prefix + "-event.csv")),
          joinPath(catalogWorkingDir, (prefix + "-phase.csv")),
          joinPath(catalogWorkingDir, (prefix + "-station.csv")));
    }
    clusterId++;
  }

  if (_saveProcessing)
  {
    relocatedCatalog->writeToFile(
        joinPath(catalogWorkingDir, "relocated-event.csv"),
        joinPath(catalogWorkingDir, "relocated-phase.csv"),
        joinPath(catalogWorkingDir, "relocated-station.csv"));
  }

  _ttt->freeResources();

  return relocatedCatalog;
}

unique_ptr<Catalog> DD::relocateSingleEvent(const Catalog &singleEvent,
                                            const ClusteringOptions &clustOpt1,
                                            const ClusteringOptions &clustOpt2,
                                            const SolverOptions &solverOpt)
{
  const Catalog &bgCat = _bgCat;

  // there must be only one event in the catalog, the origin to relocate
  const Event &evToRelocate = singleEvent.getEvents().begin()->second;
  const auto &evToRelocatePhases =
      singleEvent.getPhases().equal_range(evToRelocate.id);

  logInfo("Starting DD relocator in single event mode: event %s lat %.6f lon "
          "%.6f depth %.4f mag %.2f time %s #phases %ld",
          string(evToRelocate).c_str(), evToRelocate.latitude,
          evToRelocate.longitude, evToRelocate.depth, evToRelocate.magnitude,
          UTCClock::toString(evToRelocate.time).c_str(),
          std::distance(evToRelocatePhases.first, evToRelocatePhases.second));

  // prepare a folder for debug files
  string baseWorkingDir;
  if (_saveProcessing)
  {
    do
    {
      baseWorkingDir = generateWorkingSubDir(evToRelocate);
      baseWorkingDir = joinPath(_workingDir, baseWorkingDir);
    } while (pathExists(baseWorkingDir));

    if (!createDirectories(baseWorkingDir))
    {
      string msg = "Unable to create working directory: " + baseWorkingDir;
      throw Exception(msg);
    }
    logInfo("Working dir %s", baseWorkingDir.c_str());
  }

  // prepare file logger
  unique_ptr<Logger::File> fileLogger;
  if (_saveProcessing)
  {
    string logFile = joinPath(baseWorkingDir, "info.log");
    fileLogger =
        Logger::toFile(logFile, {Logger::Level::info, Logger::Level::warning,
                                 Logger::Level::error});
  }

  logInfo("Performing step 1: initial location refinement (no "
          "cross-correlation)");

  string eventWorkingDir = joinPath(baseWorkingDir, "step1");

  unique_ptr<Catalog> evToRelocateCat =
      Catalog::filterPhasesAndSetWeights(singleEvent, Phase::Source::RT_EVENT,
                                         _cfg.validPphases, _cfg.validSphases);

  unique_ptr<Catalog> relocatedEvCat =
      relocateEventSingleStep(bgCat, *evToRelocateCat, eventWorkingDir,
                              clustOpt1, solverOpt, false, false);

  if (relocatedEvCat)
  {
    const Event &ev = relocatedEvCat->getEvents().begin()->second;
    logInfo("Step 1 relocation successful, new location: "
            "lat %.6f lon %.6f depth %.4f time %s",
            ev.latitude, ev.longitude, ev.depth,
            UTCClock::toString(ev.time).c_str());
    logInfo("Relocation report: %s", relocationReport(*relocatedEvCat).c_str());

    evToRelocateCat = std::move(relocatedEvCat);
  }
  else
  {
    logError("Failed to perform step 1 origin relocation");
  }

  logInfo("Performing step 2: relocation with cross-correlation");

  eventWorkingDir = joinPath(baseWorkingDir, "step2");

  unique_ptr<Catalog> relocatedEvWithXcorr =
      relocateEventSingleStep(bgCat, *evToRelocateCat, eventWorkingDir,
                              clustOpt2, solverOpt, true, _useArtificialPhases);

  if (relocatedEvWithXcorr)
  {
    Event ev = relocatedEvWithXcorr->getEvents().begin()->second;
    logInfo("Step 2 relocation successful, new location: "
            "lat %.6f lon %.6f depth %.4f time %s",
            ev.latitude, ev.longitude, ev.depth,
            UTCClock::toString(ev.time).c_str());
    logInfo("Relocation report: %s",
            relocationReport(*relocatedEvWithXcorr).c_str());

    // update the "origin change information" taking into consideration
    // the first relocation step, too
    if (relocatedEvCat)
    {
      const Event &prevRelocEv = relocatedEvCat->getEvents().begin()->second;
      if (prevRelocEv.relocInfo.isRelocated)
      {
        ev.relocInfo.locChange += prevRelocEv.relocInfo.locChange;
        ev.relocInfo.depthChange += prevRelocEv.relocInfo.depthChange;
        ev.relocInfo.timeChange += prevRelocEv.relocInfo.timeChange;
        ev.relocInfo.startRms = prevRelocEv.relocInfo.startRms;
        relocatedEvWithXcorr->updateEvent(ev);
      }
    }

    logInfo("Total Changes: location=%.2f[km] depth=%.2f[km] "
            "time=%.3f[sec] Rms=%.3f[sec] (before/after %.3f/%.3f)",
            ev.relocInfo.locChange, ev.relocInfo.depthChange,
            ev.relocInfo.timeChange,
            (ev.relocInfo.finalRms - ev.relocInfo.startRms),
            ev.relocInfo.startRms, ev.relocInfo.finalRms);
  }
  else
  {
    logError("Failed to perform step 2 origin relocation");
  }

  _ttt->freeResources();

  if (!relocatedEvWithXcorr) throw Exception("Failed origin relocation");

  return relocatedEvWithXcorr;
}

unique_ptr<Catalog>
DD::relocateEventSingleStep(const Catalog &bgCat,
                            const Catalog &evToRelocateCat,
                            const string &workingDir,
                            const ClusteringOptions &clustOpt,
                            const SolverOptions &solverOpt,
                            bool doXcorr,
                            bool computeTheoreticalPhases)
{
  if (_saveProcessing)
  {
    if (!createDirectories(workingDir))
    {
      string msg = "Unable to create working directory: " + workingDir;
      throw Exception(msg);
    }
    logInfo("Working dir %s", workingDir.c_str());

    evToRelocateCat.writeToFile(
        joinPath(workingDir, "single-event.csv"),
        joinPath(workingDir, "single-event-phase.csv"),
        joinPath(workingDir, "single-event-station.csv"));
  }

  unique_ptr<Catalog> relocatedEvCat;

  try
  {
    // extract event to relocate
    const Event &evToRelocate = evToRelocateCat.getEvents().begin()->second;

    //
    // select neighbouring events
    //
    bool keepUnmatchedPhases = doXcorr; // useful for detecting missed picks

    unique_ptr<Neighbours> neighbours = selectNeighbouringEvents(
        bgCat, evToRelocate, evToRelocateCat, clustOpt.minWeight,
        clustOpt.minESdist, clustOpt.maxESdist, clustOpt.minEStoIEratio,
        clustOpt.minDTperEvt, clustOpt.maxDTperEvt, clustOpt.minNumNeigh,
        clustOpt.maxNumNeigh, clustOpt.numEllipsoids, clustOpt.maxEllipsoidSize,
        keepUnmatchedPhases);

    logInfo("Found %u neighbouring events", neighbours->ids.size());

    //
    // prepare catalog to relocate
    //
    unique_ptr<Catalog> catalog = neighbours->toCatalog(bgCat);
    unsigned evToRelocateNewId =
        catalog->add(evToRelocate.id, evToRelocateCat, false);
    neighbours->refEvId = evToRelocateNewId;

    if (_saveProcessing)
    {
      catalog->writeToFile(joinPath(workingDir, "starting-event.csv"),
                           joinPath(workingDir, "starting-phase.csv"),
                           joinPath(workingDir, "starting-station.csv"));
    }

    unordered_map<unsigned, unique_ptr<Neighbours>> neighCluster;
    neighCluster.emplace(neighbours->refEvId, std::move(neighbours));

    XCorrCache xcorr;
    if (doXcorr)
    {
      // Perform cross-correlation, which also detects picks around theoretical
      // arrival times. The catalog will be updated with the corresponding
      // phases.
      xcorr = buildXCorrCache(*catalog, neighCluster, computeTheoreticalPhases,
                              clustOpt.xcorrMaxEvStaDist,
                              clustOpt.xcorrMaxInterEvDist);
    }

    // the actual relocation
    relocatedEvCat = relocate(*catalog, neighCluster, solverOpt, true, xcorr);

    if (_saveProcessing)
    {
      relocatedEvCat->writeToFile(
          joinPath(workingDir, "relocated-event.csv"),
          joinPath(workingDir, "relocated-phase.csv"),
          joinPath(workingDir, "relocated-station.csv"));
    }
  }
  catch (exception &e)
  {
    logError("%s", e.what());
  }

  return relocatedEvCat;
}

unique_ptr<Catalog> DD::relocate(
    const Catalog &catalog,
    const unordered_map<unsigned, unique_ptr<Neighbours>> &neighCluster,
    const SolverOptions &solverOpt,
    bool keepNeighboursFixed,
    const XCorrCache &xcorr) const
{
  logInfo("Building and solving double-difference system...");

  //
  // iterate the solver computation multiple times
  //
  unique_ptr<const Catalog> finalCatalog(new Catalog(catalog));
  unordered_map<unsigned, unique_ptr<Neighbours>> finalNeighCluster;
  ObservationParams obsparams;
  for (unsigned iteration = 0; iteration < solverOpt.algoIterations;
       iteration++)
  {
    //
    // compute parameters for this loop iteration
    //
    auto interpolate = [&](double start, double end) -> double {
      if (solverOpt.algoIterations < 2) return (start + end) / 2;
      return start + (end - start) * iteration / (solverOpt.algoIterations - 1);
    };

    double dampingFactor =
        interpolate(solverOpt.dampingFactorStart, solverOpt.dampingFactorEnd);
    double downWeightingByResidual =
        interpolate(solverOpt.downWeightingByResidualStart,
                    solverOpt.downWeightingByResidualEnd);
    double absLocConstraint   = interpolate(solverOpt.absLocConstraintStart,
                                          solverOpt.absLocConstraintEnd);
    double absTTDiffObsWeight = interpolate(1.0, solverOpt.absTTDiffObsWeight);
    double xcorrObsWeight     = interpolate(1.0, solverOpt.xcorrObsWeight);

    logInfo("Solving iteration %u num events %lu. Parameters: "
            "observWeight TT/CC=%.2f/%.2f dampingFactor=%.2f "
            "downWeightingByResidual=%.2f absLocConstraint=%.2f",
            iteration, neighCluster.size(), absTTDiffObsWeight, xcorrObsWeight,
            dampingFactor, downWeightingByResidual, absLocConstraint);

    // create a solver and then add observations
    Solver solver(solverOpt.type);

    //
    // Add absolute travel time/cross-correlation differences to the solver
    // (the observations)
    //
    for (const auto &kv : neighCluster)
    {
      addObservations(solver, absTTDiffObsWeight, xcorrObsWeight, *finalCatalog,
                      *kv.second, keepNeighboursFixed,
                      solverOpt.usePickUncertainty, xcorr, obsparams);
    }
    obsparams.addToSolver(solver);

    //
    // solve the system
    //
    try
    {
      solver.solve(solverOpt.solverIterations, absLocConstraint, dampingFactor,
                   downWeightingByResidual, solverOpt.L2normalization);
    }
    catch (exception &e)
    {
      logInfo("Cannot solve the double-difference system, stop here (%s)",
              e.what());
      break;
    }

    // prepare for next iteration
    obsparams = ObservationParams();

    // update event parameters
    finalCatalog = updateRelocatedEvents(
        solver, *finalCatalog, neighCluster, obsparams,
        std::max(absTTDiffObsWeight, xcorrObsWeight), finalNeighCluster);
  }

  // compute last bit of statistics for the relocated events
  return updateRelocatedEventsFinalStats(catalog, *finalCatalog,
                                         finalNeighCluster);
}

string DD::relocationReport(const Catalog &relocatedEv)
{
  const Event &event = relocatedEv.getEvents().begin()->second;
  if (!event.relocInfo.isRelocated) return "Event not relocated";

  return strf("Origin changes: location=%.2f[km] depth=%.2f[km] time=%.3f[sec] "
              "Rms change [sec]: %.3f (before/after %.3f/%.3f) "
              "Neighbours=%u Used Phases: P=%u S=%u "
              "Stations distance [km]: min=%.1f median=%.1f max=%.1f "
              "DD observations: %u (CC P/S %u/%u TT P/S %u/%u) "
              "DD residuals [msec]: before=%.f+/-%.1f after=%.f+/-%.1f",
              event.relocInfo.locChange, event.relocInfo.depthChange,
              event.relocInfo.timeChange,
              (event.relocInfo.finalRms - event.relocInfo.startRms),
              event.relocInfo.startRms, event.relocInfo.finalRms,
              event.relocInfo.numNeighbours, event.relocInfo.phases.usedP,
              event.relocInfo.phases.usedS,
              event.relocInfo.phases.stationDistMin,
              event.relocInfo.phases.stationDistMedian,
              event.relocInfo.phases.stationDistMax,
              (event.relocInfo.dd.numCCp + event.relocInfo.dd.numCCs +
               event.relocInfo.dd.numTTp + event.relocInfo.dd.numTTs),
              event.relocInfo.dd.numCCp, event.relocInfo.dd.numCCs,
              event.relocInfo.dd.numTTp, event.relocInfo.dd.numTTs,
              event.relocInfo.dd.startResidualMedian * 1000,
              event.relocInfo.dd.startResidualMAD * 1000,
              event.relocInfo.dd.finalResidualMedian * 1000,
              event.relocInfo.dd.finalResidualMAD * 1000);
}

/*
 * Add both the absolute travel time differences and the differential
 * travel times from the cross-correlation for pairs of earthquakes to the
 * solver.
 */
void DD::addObservations(Solver &solver,
                         double absTTDiffObsWeight,
                         double xcorrObsWeight,
                         const Catalog &catalog,
                         const Neighbours &neighbours,
                         bool keepNeighboursFixed,
                         bool usePickUncertainty,
                         const XCorrCache &xcorr,
                         ObservationParams &obsparams) const
{
  // copy event because we'll update it
  const Event &refEv = catalog.getEvents().at(neighbours.refEvId);

  //
  // loop through reference event phases
  //
  auto eqlrng = catalog.getPhases().equal_range(refEv.id);
  for (auto it = eqlrng.first; it != eqlrng.second; ++it)
  {
    const Phase &refPhase  = it->second;
    const Station &station = catalog.getStations().at(refPhase.stationId);
    char phaseTypeAsChar   = static_cast<char>(refPhase.procInfo.type);

    //
    // loop through neighbouring events and look for the matching phase
    //
    for (unsigned neighEvId : neighbours.ids)
    {
      const Event &event = catalog.getEvents().at(neighEvId);

      if (!neighbours.has(neighEvId, refPhase.stationId,
                          refPhase.procInfo.type))
        continue;

      const Phase &phase =
          catalog
              .searchPhase(event.id, refPhase.stationId, refPhase.procInfo.type)
              ->second;
      //
      // compute travel times for both event and `refEv`
      //
      UTCTime::duration ref_travel_time = refPhase.time - refEv.time;
      if (ref_travel_time.count() < 0)
      {
        logDebug("Ignoring phase %s with negative travel time",
                 string(refPhase).c_str());
        continue;
      }

      UTCTime::duration travel_time = phase.time - event.time;
      if (travel_time.count() < 0)
      {
        logDebug("Ignoring phase %s with negative travel time",
                 string(phase).c_str());
        continue;
      }

      if (!obsparams.add(*_ttt, refEv, station, refPhase, true) ||
          !obsparams.add(*_ttt, event, station, phase, !keepNeighboursFixed))
      {
        logDebug("Skipping observation (ev %u-%u sta %s phase %c)", refEv.id,
                 event.id, station.id.c_str(), phaseTypeAsChar);
        continue;
      }

      //
      // compute absolute travel time differences to the solver
      //
      duration<double> diffTime = ref_travel_time - travel_time;
      double weight =
          usePickUncertainty
              ? (refPhase.procInfo.weight + phase.procInfo.weight) / 2.0
              : 1.0;
      bool isXcorr = false;

      //
      // Check if we have cross-correlation results for the current
      // event/`refEv` pair at station/phase and use those instead.
      //
      if (xcorr.has(refEv.id, event.id, refPhase.stationId,
                    refPhase.procInfo.type))
      {

        const auto &xcdata = xcorr.get(refEv.id, event.id, refPhase.stationId,
                                       refPhase.procInfo.type);
        diffTime -= duration<double>(xcdata.lag);
        weight *= xcorrObsWeight;
        isXcorr = true;
      }
      else
      {
        weight *= absTTDiffObsWeight;
      }

      solver.addObservation(refEv.id, event.id, refPhase.stationId,
                            phaseTypeAsChar, diffTime.count(), weight, isXcorr);
    }
  }
}

bool DD::ObservationParams::add(HDD::TravelTimeTable &ttt,
                                const Event &event,
                                const Station &station,
                                const Phase &phase,
                                bool computeEvChanges)
{
  char phaseType = static_cast<char>(phase.procInfo.type);
  const string key =
      std::to_string(event.id) + "@" + station.id + ":" + phaseType;
  if (_entries.find(key) == _entries.end())
  {
    try
    {
      double travelTime, takeOfAngleAzim, takeOfAngleDip, velocityAtSrc;
      ttt.compute(event, station, string(1, phaseType), travelTime,
                  takeOfAngleAzim, takeOfAngleDip, velocityAtSrc);
      double ttResidual = travelTime - durToSec(phase.time - event.time);
      _entries[key]     = Entry{event,          station,       phaseType,
                            travelTime,     ttResidual,    takeOfAngleAzim,
                            takeOfAngleDip, velocityAtSrc, computeEvChanges};
    }
    catch (exception &e)
    {
      logWarning(
          "Travel Time Table error: %s (Event lat %.6f lon %.6f depth %.6f "
          "Station lat %.6f lon %.6f elevation %.f )",
          e.what(), event.latitude, event.longitude, event.depth,
          station.latitude, station.longitude, station.elevation);
      return false;
    }
  }
  return true;
}

const DD::ObservationParams::Entry &DD::ObservationParams::get(
    unsigned eventId, const string stationId, char phaseType) const
{
  const string key =
      std::to_string(eventId) + "@" + stationId + ":" + phaseType;
  return _entries.at(key);
}

void DD::ObservationParams::addToSolver(Solver &solver) const
{
  for (const auto &kv : _entries)
  {
    const ObservationParams::Entry &e = kv.second;
    solver.addObservationParams(
        e.event.id, e.station.id, e.phaseType, e.event.latitude,
        e.event.longitude, e.event.depth, e.station.latitude,
        e.station.longitude, e.station.elevation, e.computeEvChanges,
        e.travelTime, e.travelTimeResidual, e.takeOfAngleAzim, e.takeOfAngleDip,
        e.velocityAtSrc);
  }
}

unique_ptr<Catalog> DD::updateRelocatedEvents(
    const Solver &solver,
    const Catalog &catalog,
    const unordered_map<unsigned, unique_ptr<Neighbours>> &neighCluster,
    ObservationParams &obsparams,
    double pickWeightScaler,
    unordered_map<unsigned, unique_ptr<Neighbours>> &finalNeighCluster // output
) const
{
  unordered_map<string, Station> stations    = catalog.getStations();
  map<unsigned, Event> events                = catalog.getEvents();
  unordered_multimap<unsigned, Phase> phases = catalog.getPhases();
  unsigned relocatedEvs                      = 0;
  vector<double> allRms;

  //
  // loop through each event with its corresponding neighbours cluster
  //
  for (const auto &kv : neighCluster)
  {
    Event &event = events.at(kv.second->refEvId);

    // get relocation changes computed by the solver for the current event
    double deltaLat, deltaLon, deltaDepth, deltaTT;
    if (!solver.getEventChanges(event.id, deltaLat, deltaLon, deltaDepth,
                                deltaTT))
    {
      event.relocInfo.isRelocated = false;
      continue;
    }

    // check for airquakes (can we trust ttt/vel.model above surface?)
    if (event.depth + deltaDepth < 0)
    {
      // allow 100 meters change
      if (deltaDepth > 0.100)
      {
        logDebug("Ignoring airquake event %s", string(event).c_str());
        continue;
      }
      // do not move the depth in this case
      deltaDepth = 0;
    }

    unique_ptr<Neighbours> finalNeighbours(new Neighbours());
    finalNeighbours->refEvId = event.id;
    bool isFirstIteration    = !event.relocInfo.isRelocated;

    //
    // update event location/time and compute statistics
    //
    relocatedEvs++;
    event.latitude += deltaLat;
    event.longitude += deltaLon;
    event.depth += deltaDepth;
    event.time += secToDur(deltaTT);
    event.relocInfo.finalRms      = 0;
    event.relocInfo.isRelocated   = true;
    event.relocInfo.numNeighbours = 0;
    event.relocInfo.phases        = {0};
    event.relocInfo.dd.numTTp     = 0;
    event.relocInfo.dd.numTTs     = 0;
    event.relocInfo.dd.numCCp     = 0;
    event.relocInfo.dd.numCCs     = 0;

    set<unsigned> neighbourIds;
    vector<double> obsResiduals;
    unsigned rmsCount = 0;
    auto eqlrng       = phases.equal_range(event.id);
    for (auto it = eqlrng.first; it != eqlrng.second; ++it)
    {
      Phase &phase           = it->second;
      const Station &station = stations.at(phase.stationId);
      char phaseTypeAsChar   = static_cast<char>(phase.procInfo.type);

      phase.relocInfo.isRelocated = false;

      unsigned startTTObs, startCCObs, finalTotalObs;
      double meanDDResidual, meanAPrioriWeight, meanFinalWeight;

      if (!solver.getObservationParamsChanges(
              event.id, station.id, phaseTypeAsChar, startTTObs, startCCObs,
              finalTotalObs, meanAPrioriWeight, meanFinalWeight, meanDDResidual,
              neighbourIds))
      {
        continue;
      }

      if (finalTotalObs == 0) continue;

      phase.relocInfo.isRelocated = true;
      phase.relocInfo.finalWeight =
          meanFinalWeight / pickWeightScaler; // range 0-1
      phase.relocInfo.numTTObs = startTTObs;
      phase.relocInfo.numCCObs = startCCObs;
      if (isFirstIteration)
        phase.relocInfo.startMeanDDResidual = meanDDResidual;
      phase.relocInfo.finalMeanDDResidual = meanDDResidual;
      obsResiduals.push_back(meanDDResidual);

      if (obsparams.add(*_ttt, event, station, phase, true))
      {
        double travelTime =
            obsparams.get(event.id, station.id, phaseTypeAsChar).travelTime;
        phase.relocInfo.finalTTResidual =
            travelTime - durToSec(phase.time - event.time);
        rmsCount++;
      }
      else
      {
        phase.relocInfo.finalTTResidual = 0;
      }

      event.relocInfo.finalRms += square(phase.relocInfo.finalTTResidual);
      if (phase.procInfo.type == Phase::Type::P)
      {
        event.relocInfo.phases.usedP++;
        event.relocInfo.dd.numCCp += phase.relocInfo.numCCObs;
        event.relocInfo.dd.numTTp += phase.relocInfo.numTTObs;
      }
      if (phase.procInfo.type == Phase::Type::S)
      {
        event.relocInfo.phases.usedS++;
        event.relocInfo.dd.numCCs += phase.relocInfo.numCCObs;
        event.relocInfo.dd.numTTs += phase.relocInfo.numTTObs;
      }

      for (unsigned nId : neighbourIds)
      {
        finalNeighbours->add(nId, station.id, phase.procInfo.type);
      }
    }

    if (rmsCount > 0)
    {
      event.relocInfo.finalRms = std::sqrt(event.relocInfo.finalRms / rmsCount);
      allRms.push_back(event.relocInfo.finalRms);
    }

    double residualMedian = computeMedian(obsResiduals);
    double residualMAD =
        computeMedianAbsoluteDeviation(obsResiduals, residualMedian);
    if (isFirstIteration)
    {
      event.relocInfo.dd.startResidualMedian = residualMedian;
      event.relocInfo.dd.startResidualMAD    = residualMAD;
    }
    event.relocInfo.dd.finalResidualMedian = residualMedian;
    event.relocInfo.dd.finalResidualMAD    = residualMAD;

    event.relocInfo.numNeighbours               = finalNeighbours->ids.size();
    finalNeighCluster[finalNeighbours->refEvId] = std::move(finalNeighbours);
  }

  const double allRmsMedian = computeMedian(allRms);
  const double allRmsMAD = computeMedianAbsoluteDeviation(allRms, allRmsMedian);

  logInfo("Successfully relocated %u events, RMS median %.4f [sec] median "
          "absolute deviation %.4f [sec]",
          relocatedEvs, allRmsMedian, allRmsMAD);

  return unique_ptr<Catalog>(new Catalog(stations, events, phases));
}

unique_ptr<Catalog> DD::updateRelocatedEventsFinalStats(
    const Catalog &startCatalog,
    const Catalog &finalCatalog,
    const unordered_map<unsigned, unique_ptr<Neighbours>> &neighCluster) const
{
  unique_ptr<Catalog> catalogToReturn(new Catalog());
  vector<double> allRms;
  vector<double> stationDist;

  for (const auto &kv : neighCluster)
  {
    const unique_ptr<Neighbours> &neighbours = kv.second;
    auto it = finalCatalog.getEvents().find(neighbours->refEvId);

    // If the event hasn't been relocated, remove it from the final catalog.
    if (it == finalCatalog.getEvents().end() ||
        !it->second.relocInfo.isRelocated)
    {
      continue;
    }

    unique_ptr<Catalog> tmpCat =
        finalCatalog.extractEvent(neighbours->refEvId, true);

    const Event &startEvent = startCatalog.getEvents().at(neighbours->refEvId);
    Event finalEvent        = tmpCat->getEvents().at(neighbours->refEvId);

    //
    // Compute location/time change of start/final origin.
    //
    finalEvent.relocInfo.locChange =
        computeDistance(finalEvent.latitude, finalEvent.longitude,
                        startEvent.latitude, startEvent.longitude);
    finalEvent.relocInfo.depthChange = finalEvent.depth - startEvent.depth;
    finalEvent.relocInfo.timeChange =
        durToSec(finalEvent.time - startEvent.time);

    //
    // Compute starting event rms considering only the phases in the final
    // catalog.
    //
    unsigned rmsCount             = 0;
    finalEvent.relocInfo.startRms = 0;
    auto eqlrng = tmpCat->getPhases().equal_range(finalEvent.id);
    for (auto it = eqlrng.first; it != eqlrng.second; ++it)
    {
      Phase finalPhase       = it->second;
      const Station &station = tmpCat->getStations().at(finalPhase.stationId);
      try
      {
        double travelTime;
        _ttt->compute(startEvent, station,
                      string(1, static_cast<char>(finalPhase.procInfo.type)),
                      travelTime);
        double residual =
            travelTime - durToSec(finalPhase.time - startEvent.time);
        finalPhase.relocInfo.startTTResidual = residual;
        tmpCat->updatePhase(finalPhase, false);
        finalEvent.relocInfo.startRms += residual * residual;
        rmsCount++;
      }
      catch (exception &e)
      {
        logWarning(
            "Travel Time Table error: %s (Event lat %.6f lon %.6f depth %.6f "
            "Station lat %.6f lon %.6f elevation %.f )",
            e.what(), startEvent.latitude, startEvent.longitude,
            startEvent.depth, station.latitude, station.longitude,
            station.elevation);
      }
    }

    if (rmsCount > 0)
    {
      finalEvent.relocInfo.startRms =
          std::sqrt(finalEvent.relocInfo.startRms / rmsCount);
      allRms.push_back(finalEvent.relocInfo.startRms);
    }

    //
    // compute station distances to final event
    //
    stationDist.clear();
    for (const auto &kv : neighbours->allPhases())
    {
      const string &stationId = kv.first;
      for (Phase::Type phaseType : kv.second)
      {
        // Check if this station is actually part of the event phases (remember
        // keepUnmatchedPhases option during clustering). Make sure that the
        // phase was used for relocation.
        auto it = tmpCat->searchPhase(finalEvent.id, stationId, phaseType);
        if (it != tmpCat->getPhases().end() && it->second.relocInfo.isRelocated)
        {
          const Station &station =
              tmpCat->getStations().at(it->second.stationId);
          stationDist.push_back(computeDistance(finalEvent, station));
          break;
        }
      }
    }

    finalEvent.relocInfo.phases.stationDistMedian = computeMedian(stationDist);
    auto min_max = std::minmax_element(stationDist.begin(), stationDist.end());
    finalEvent.relocInfo.phases.stationDistMin = *min_max.first;
    finalEvent.relocInfo.phases.stationDistMax = *min_max.second;

    tmpCat->updateEvent(finalEvent, false);
    catalogToReturn->add(finalEvent.id, *tmpCat, true);
  }

  const double allRmsMedian = computeMedian(allRms);
  const double allRmsMAD = computeMedianAbsoluteDeviation(allRms, allRmsMedian);

  logInfo("Events RMS before relocation: median %.4f median absolute "
          "deviation %.4f",
          allRmsMedian, allRmsMAD);

  return catalogToReturn;
}

void DD::addMissingEventPhases(const Event &refEv,
                               Catalog &refEvCatalog,
                               const Catalog &searchCatalog,
                               const Neighbours &neighbours)
{
  vector<Phase> newPhases =
      findMissingEventPhases(refEv, refEvCatalog, searchCatalog, neighbours);

  for (Phase &ph : newPhases)
  {
    refEvCatalog.updatePhase(ph, true);
    const Station &station = searchCatalog.getStations().at(ph.stationId);
    refEvCatalog.addStation(station);
  }
}

vector<Phase> DD::findMissingEventPhases(const Event &refEv,
                                         Catalog &refEvCatalog,
                                         const Catalog &searchCatalog,
                                         const Neighbours &neighbours)
{
  //
  // find stations for which the `refEv` doesn't have phases
  //
  vector<MissingStationPhase> missingPhases =
      getMissingPhases(refEv, refEvCatalog, searchCatalog);

  //
  // for each missed phase try to detect it
  //
  vector<Phase> newPhases;
  for (const MissingStationPhase &pair : missingPhases)
  {
    const Station &station      = searchCatalog.getStations().at(pair.first);
    const Phase::Type phaseType = pair.second;

    //
    // Loop through every other event and select the ones who have a manually
    // picked phase for the missing station.
    //
    vector<DD::PhasePeer> peers =
        findPhasePeers(station, phaseType, searchCatalog, neighbours);
    if (peers.size() <= 0)
    {
      continue;
    }

    // compute velocity using existing background catalog phases
    double phaseVelocity = 0;
    for (const PhasePeer &peer : peers)
    {
      const Event &event     = peer.first;
      const Phase &phase     = peer.second;
      double travelTime      = durToSec(phase.time - event.time);
      double stationDistance = computeDistance(event, station);
      double vel             = stationDistance / travelTime;
      phaseVelocity += vel;
    }
    phaseVelocity /= peers.size();

    Phase refEvNewPhase =
        createThoreticalPhase(station, phaseType, refEv, peers, phaseVelocity);

    newPhases.push_back(refEvNewPhase);
  }

  return newPhases;
}

vector<DD::MissingStationPhase>
DD::getMissingPhases(const Event &refEv,
                     Catalog &refEvCatalog,
                     const Catalog &searchCatalog) const
{
  const auto &refEvPhases = refEvCatalog.getPhases().equal_range(refEv.id);

  //
  // Loop through stations and find those for which the `refEv` doesn't have
  // phases.
  //
  vector<MissingStationPhase> missingPhases;
  for (const auto &kv : searchCatalog.getStations())
  {
    const Station &station = kv.second;

    bool foundP = false, foundS = false;
    for (auto it = refEvPhases.first; it != refEvPhases.second; ++it)
    {
      const Phase &phase = it->second;
      if (station.networkCode == phase.networkCode &&
          station.stationCode == phase.stationCode &&
          station.locationCode == phase.locationCode)
      {
        if (phase.procInfo.type == Phase::Type::P) foundP = true;
        if (phase.procInfo.type == Phase::Type::S) foundS = true;
      }
      if (foundP and foundS) break;
    }
    if (!foundP || !foundS)
    {
      if (!foundP)
        missingPhases.push_back(
            MissingStationPhase(station.id, Phase::Type::P));
      if (!foundS)
        missingPhases.push_back(
            MissingStationPhase(station.id, Phase::Type::S));
    }
  }

  return missingPhases;
}

vector<DD::PhasePeer> DD::findPhasePeers(const Station &station,
                                         const Phase::Type &phaseType,
                                         const Catalog &searchCatalog,
                                         const Neighbours &neighbours) const
{
  //
  // Loop through every other event and select those manual phases of the
  // `station` we are interested in.
  //
  vector<PhasePeer> phasePeers;

  for (unsigned neighEvId : neighbours.ids)
  {
    const Event &event = searchCatalog.getEvents().at(neighEvId);

    if (neighbours.has(neighEvId, station.id, phaseType))
    {
      const Phase &phase =
          searchCatalog.searchPhase(neighEvId, station.id, phaseType)->second;

      if (station.networkCode == phase.networkCode &&
          station.stationCode == phase.stationCode &&
          station.locationCode == phase.locationCode &&
          phaseType == phase.procInfo.type)
      {
        if (phase.isManual)
        {
          phasePeers.push_back(PhasePeer(event, phase));
        }
        break;
      }
    }
  }

  return phasePeers;
}

Phase DD::createThoreticalPhase(const Station &station,
                                const Phase::Type &phaseType,
                                const Event &refEv,
                                const vector<DD::PhasePeer> &peers,
                                double phaseVelocity)
{
  const auto xcorrCfg = _cfg.xcorr.at(phaseType);

  // store most recent `channelCode` used
  struct
  {
    string channelCode;
    UTCTime time;
  } streamInfo = {"", UTCTime()};

  for (const PhasePeer &peer : peers)
  {
    // const Event& event = peer.first;
    const Phase &phase = peer.second;
    // get the closest stream to the `refEv` (w.r.t. time)
    if (std::abs((refEv.time - phase.time).count()) <
        std::abs((refEv.time - streamInfo.time).count()))
      streamInfo = {phase.channelCode, phase.time};
  }

  // initialize the new phase
  Phase refEvNewPhase;

  refEvNewPhase.eventId      = refEv.id;
  refEvNewPhase.stationId    = station.id;
  refEvNewPhase.networkCode  = station.networkCode;
  refEvNewPhase.stationCode  = station.stationCode;
  refEvNewPhase.locationCode = station.locationCode;
  refEvNewPhase.channelCode =
      getBandAndInstrumentCodes(streamInfo.channelCode) +
      xcorrCfg.components[0];
  refEvNewPhase.isManual      = false;
  refEvNewPhase.procInfo.type = phaseType;

  // use phase velocity to compute phase time
  double stationDistance = computeDistance(refEv, station);
  refEvNewPhase.time = refEv.time + secToDur(stationDistance / phaseVelocity);

  refEvNewPhase.lowerUncertainty = Catalog::DEFAULT_AUTOMATIC_PICK_UNCERTAINTY;
  refEvNewPhase.upperUncertainty = Catalog::DEFAULT_AUTOMATIC_PICK_UNCERTAINTY;
  refEvNewPhase.procInfo.weight  = Catalog::computePickWeight(refEvNewPhase);
  refEvNewPhase.procInfo.source  = Phase::Source::THEORETICAL;
  refEvNewPhase.type             = strf("%ct", static_cast<char>(phaseType));

  return refEvNewPhase;
}

XCorrCache DD::buildXCorrCache(
    Catalog &catalog,
    const unordered_map<unsigned, unique_ptr<Neighbours>> &neighCluster,
    bool computeTheoreticalPhases,
    double xcorrMaxEvStaDist,
    double xcorrMaxInterEvDist)
{
  logInfo("Computing differential times via cross-correlation...");

  XCorrCache xcorr;
  _counters.reset();

  WfCounters wfcount{0};

  unsigned long performed = 0;

  for (const auto &kv : neighCluster)
  {
    const unique_ptr<Neighbours> &neighbours = kv.second;
    const Event &refEv = catalog.getEvents().at(neighbours->refEvId);

    // Compute theoretical phases for stations that have no picks. The
    // cross-correlation will be used to detect and fix the pick time.
    if (computeTheoreticalPhases)
    {
      addMissingEventPhases(refEv, catalog, catalog, *neighbours);
    }

    buildXcorrDiffTTimePairs(catalog, *neighbours, refEv, xcorrMaxEvStaDist,
                             xcorrMaxInterEvDist, xcorr);

    // Update theoretical and automatic phase pick time and uncertainties based
    // on cross-correlation results. Also, drop theoretical phases wihout any
    // good cross-correlation result.
    fixPhases(catalog, refEv, xcorr);

    const unsigned onePerThousand =
        neighCluster.size() < 1000 ? 1 : (neighCluster.size() / 1000);
    if (++performed % onePerThousand == 0)
    {
      logInfo("Cross-correlation completion %.1f%%",
              (performed * 100 / (double)neighCluster.size()));
    }
  }

  wfcount.update(_wfAccess.loader, _wfAccess.diskCache, _wfAccess.snrFilter);

  logInfo("Catalog waveform data: waveforms downloaded %u, not available "
          "%u, loaded from disk cache %u. Waveforms with SNR too low %u",
          wfcount.downloaded, wfcount.no_avail, wfcount.disk_cached,
          wfcount.snr_low);

  printCounters();

  return xcorr;
}

/*
 * Compute and store to `XCorrCache` cross-correlated differential travel times
 * for pairs of the earthquake.
 */
void DD::buildXcorrDiffTTimePairs(Catalog &catalog,
                                  const Neighbours &neighbours,
                                  const Event &refEv,
                                  double xcorrMaxEvStaDist,
                                  double xcorrMaxInterEvDist,
                                  XCorrCache &xcorr)
{
  logDebug("Computing cross-correlation differential travel times for event %s",
           string(refEv).c_str());
  //
  // Prepare the waveform loaders for single-event. Since they are
  // temporary and discarded after the relocation we don't want to
  // make use of the background catalog caching and keep them in
  // memory forever
  //
  shared_ptr<Waveform::Processor> preloaded(preloadNonCatalogWaveforms(
      catalog, neighbours, refEv, xcorrMaxEvStaDist, xcorrMaxInterEvDist));
  shared_ptr<Waveform::Processor> seWfLdrNoSnr; // loader with not SNR check
  shared_ptr<Waveform::Processor> seWfLdr;   // same loader but with SNR check
  shared_ptr<Waveform::SnrFilterPrc> snrLdr; // just for keeping track of stats

  if (preloaded)
  {
    seWfLdrNoSnr.reset(new Waveform::MemCachedProc(preloaded));
    seWfLdr = seWfLdrNoSnr;
    if (_cfg.snr.minSnr > 0)
    {
      snrLdr.reset(new Waveform::SnrFilterPrc(
          seWfLdrNoSnr, _cfg.snr.minSnr, _cfg.snr.noiseStart, _cfg.snr.noiseEnd,
          _cfg.snr.signalStart, _cfg.snr.signalEnd));
      seWfLdr.reset(new Waveform::MemCachedProc(snrLdr));
    }
  }

  // keep track of the `refEv` distance to stations
  multimap<double, string> stationByDistance; // <distance, stationid>
  unordered_set<string> computedStations;

  //
  // loop through reference event phases
  //
  auto eqlrngRef = catalog.getPhases().equal_range(refEv.id);
  for (auto itRef = eqlrngRef.first; itRef != eqlrngRef.second; ++itRef)
  {
    const Phase &refPhase  = itRef->second;
    const Station &station = catalog.getStations().at(refPhase.stationId);

    //
    // skip stations too far away
    //
    double stationDistance = computeDistance(refEv, station);
    if (stationDistance > xcorrMaxEvStaDist && xcorrMaxEvStaDist >= 0) continue;

    //
    // select appropriate waveform loader for refPhase
    //
    shared_ptr<Waveform::Processor> refPrc;
    if (refPhase.procInfo.source == Phase::Source::CATALOG)
      refPrc = _wfAccess.memCache;
    else if (refPhase.isManual)
      refPrc = seWfLdr;
    else // not from catalog, not manual
    {
      // For "untrusted" phases the SNR is checked AFTER the cross-correlation
      // once the pick time has been adjusted using the lag computed by the
      // cross-corr against a "trusted" phase (manual or catalog).
      refPrc = seWfLdrNoSnr;
    }

    //
    // loop through neighbouring events and cross-correlate with `refPhase`
    //
    for (unsigned neighEvId : neighbours.ids)
    {
      const Event &event = catalog.getEvents().at(neighEvId);

      //
      // skip events too far away
      //
      double interEventDistance = computeDistance(refEv, event);
      if (interEventDistance > xcorrMaxInterEvDist && xcorrMaxInterEvDist >= 0)
        continue;

      if (neighbours.has(neighEvId, refPhase.stationId, refPhase.procInfo.type))
      {
        const Phase &phase = catalog
                                 .searchPhase(event.id, refPhase.stationId,
                                              refPhase.procInfo.type)
                                 ->second;

        // In single-event mode `refPhase` is real-time and `phase` is from the
        // catalog. In multi-event mode both are from the catalog.
        if (phase.procInfo.source != Phase::Source::CATALOG)
          throw Exception("Internal logic error: phase is not from catalog");

        double coeff, lag;
        if (xcorrPhases(refEv, refPhase, *refPrc, event, phase,
                        *_wfAccess.memCache, coeff, lag))
        {
          bool goodSNR = true;

          // Check the SNR (using the pick time adjusted with the
          // cross-correlation lag) of those phases, where the check hadn't
          // been performed, yet.
          if (_cfg.snr.minSnr > 0 && !refPhase.isManual &&
              refPhase.procInfo.source != Phase::Source::CATALOG)
          {
            const auto xcorrCfg = _cfg.xcorr.at(refPhase.procInfo.type);

            // Make sure that at least one of the components allowed for this
            // phase type has a good SNR.
            goodSNR = false;
            for (const string &component : xcorrCfg.components)
            {
              UTCTime adjustedPickTime = refPhase.time - secToDur(lag);
              TimeWindow snrWin =
                  _wfAccess.snrFilter->snrTimeWindow(adjustedPickTime);

              shared_ptr<const Trace> trace = getWaveform(
                  *seWfLdrNoSnr, snrWin, refEv, refPhase, component);

              if (trace &&
                  _wfAccess.snrFilter->goodSnr(*trace, adjustedPickTime))
              {
                goodSNR = true;
                break;
              }
            }
          }

          // store good cross-correlation results
          if (goodSNR)
          {
            auto &entry1 = xcorr.getForUpdate(refEv.id, refPhase.stationId,
                                              refPhase.procInfo.type);
            entry1.update(event, phase, coeff, lag);
            auto &entry2 = xcorr.getForUpdate(event.id, phase.stationId,
                                              phase.procInfo.type);
            entry2.update(refEv, refPhase, coeff, lag);
          }
        }

        // keep track of events/station distance for every cross-correlation
        // performed
        if (computedStations.find(refPhase.stationId) == computedStations.end())
        {
          stationByDistance.emplace(stationDistance, refPhase.stationId);
          computedStations.insert(refPhase.stationId);
        }
      }
    }

    // finalize statistics
    if (xcorr.has(refEv.id, refPhase.stationId, refPhase.procInfo.type))
    {
      auto &entry = xcorr.getForUpdate(refEv.id, refPhase.stationId,
                                       refPhase.procInfo.type);
      entry.computeStats();
    }
  }

  //
  // print some useful information
  //
  if (snrLdr)
  {
    logInfo("Event %s: waveforms with SNR too low %d", string(refEv).c_str(),
            snrLdr->_counters_wf_snr_low);
  }

  for (const auto &kv : stationByDistance)
  {
    const double stationDistance = kv.first;
    const Station &station       = catalog.getStations().at(kv.second);

    bool goodPXcorr = xcorr.has(refEv.id, station.id, Phase::Type::P);
    bool goodSXcorr = xcorr.has(refEv.id, station.id, Phase::Type::S);

    if (!goodPXcorr && !goodSXcorr)
    {
      logDebug(
          "xcorr: event %5s sta %4s %5s dist %7.2f [km] - low corr coeff pairs",
          string(refEv).c_str(), station.networkCode.c_str(),
          station.stationCode.c_str(), stationDistance);
    }
    else
    {
      if (goodPXcorr)
      {
        const auto &pdata = xcorr.get(refEv.id, station.id, Phase::Type::P);
        logDebug("xcorr: event %5s sta %4s %5s dist %7.2f [km] - "
                 "%d P phases, mean coeff %.2f lag %.2f (events: %s)",
                 string(refEv).c_str(), station.networkCode.c_str(),
                 station.stationCode.c_str(), stationDistance, pdata.ccCount,
                 pdata.mean_coeff, pdata.mean_lag, pdata.peersStr.c_str());
      }
      if (goodSXcorr)
      {
        const auto &sdata = xcorr.get(refEv.id, station.id, Phase::Type::S);
        logDebug("xcorr: event %5s sta %4s %5s dist %7.2f [km] - "
                 "%d S phases, mean coeff %.2f lag %.2f (events: %s)",
                 string(refEv).c_str(), station.networkCode.c_str(),
                 station.stationCode.c_str(), stationDistance, sdata.ccCount,
                 sdata.mean_coeff, sdata.mean_lag, sdata.peersStr.c_str());
      }
    }
  }
}

shared_ptr<Waveform::Processor>
DD::preloadNonCatalogWaveforms(Catalog &catalog,
                               const Neighbours &neighbours,
                               const Event &refEv,
                               double xcorrMaxEvStaDist,
                               double xcorrMaxInterEvDist)
{
  //
  // For single-event relocation in real-time we want to load the waveforms
  // in batch otherwise the seedlink server gets stuck and becomes unresponsive
  // due to the multiple connections requests, one for each event phase
  //
  shared_ptr<Waveform::BatchLoader> batchLoader(
      new Waveform::BatchLoader(_cfg.recordStreamURL));

  shared_ptr<Waveform::Loader> loader = batchLoader;
  shared_ptr<Waveform::DiskCachedLoader> diskLoader;
  if (_useCatalogWaveformDiskCache && _waveformCacheAll)
  {
    diskLoader.reset(new Waveform::DiskCachedLoader(batchLoader, _tmpCacheDir));
    loader.reset(new Waveform::ExtraLenLoader(diskLoader, DISK_TRACE_MIN_LEN));
  }

  // Load a large enough waveform to be able to check the SNR after
  // the updating of the pick time
  if (_cfg.snr.minSnr > 0)
  {
    double maxDelay = std::max({_cfg.xcorr.at(Phase::Type::P).maxDelay,
                                _cfg.xcorr.at(Phase::Type::S).maxDelay});
    double extraBefore =
        std::min({_cfg.snr.noiseStart, _cfg.snr.signalStart}) - maxDelay;
    extraBefore = extraBefore > 0 ? 0 : std::abs(extraBefore);
    double extraAfter =
        std::max({_cfg.snr.noiseEnd, _cfg.snr.signalEnd}) + maxDelay;
    extraAfter = extraAfter < 0 ? 0 : extraAfter;
    loader.reset(new Waveform::ExtraLenLoader(loader, extraBefore, extraAfter));
  }

  shared_ptr<Waveform::Processor> proc(new Waveform::BasicProcessor(loader));

  //
  // loop through reference event phases
  //
  bool onlyCatalogPhases = true;
  auto eqlrngRef         = catalog.getPhases().equal_range(refEv.id);
  for (auto itRef = eqlrngRef.first; itRef != eqlrngRef.second; ++itRef)
  {
    const Phase &refPhase  = itRef->second;
    const Station &station = catalog.getStations().at(refPhase.stationId);

    // We deal only with real-time event data
    if (refPhase.procInfo.source == Phase::Source::CATALOG) continue;
    onlyCatalogPhases = false;

    //
    // skip stations too far away
    //
    double stationDistance = computeDistance(refEv, station);
    if (stationDistance > xcorrMaxEvStaDist && xcorrMaxEvStaDist >= 0) continue;

    //
    // loop through neighbouring events and cross-correlate with `refPhase`
    //
    for (unsigned neighEvId : neighbours.ids)
    {
      const Event &event = catalog.getEvents().at(neighEvId);

      //
      // skip events too far away
      //
      double interEventDistance = computeDistance(refEv, event);
      if (interEventDistance > xcorrMaxInterEvDist && xcorrMaxInterEvDist >= 0)
        continue;

      if (neighbours.has(neighEvId, refPhase.stationId, refPhase.procInfo.type))
      {
        //
        // For each match load the reference event phase waveforms
        //
        TimeWindow tw       = xcorrTimeWindowLong(refPhase);
        const auto xcorrCfg = _cfg.xcorr.at(refPhase.procInfo.type);

        for (const string &component : xcorrCfg.components)
        {
          // This doesn't really load the trace but force the request to reach
          // the loader, which will load all the traces later
          getWaveform(*proc, tw, refEv, refPhase, component);
        }
      }
    }
  }

  if (onlyCatalogPhases)
  {
    return nullptr;
  }

  // This will actually dowanload the waveworms all at once
  batchLoader->load();

  // print counters
  WfCounters wfcount{0};
  wfcount.update(batchLoader);
  wfcount.update(diskLoader);
  logInfo("Event %s: waveforms downloaded %u, not available %u, loaded from "
          "disk cache %u",
          string(refEv).c_str(), wfcount.downloaded, wfcount.no_avail,
          wfcount.disk_cached);
  return proc;
}

/*
 * Update theoretical and automatic phase pick times and uncertainties based on
 * cross-correlation results. Drop theoretical phases not passing the
 * cross-correlation verification.
 */
void DD::fixPhases(Catalog &catalog, const Event &refEv, XCorrCache &xcorr)
{
  unsigned totP = 0, totS = 0;
  unsigned newP = 0, newS = 0;

  vector<Phase> phasesToBeRemoved;
  vector<Phase> newPhases;

  auto eqlrng = catalog.getPhases().equal_range(refEv.id);
  for (auto it = eqlrng.first; it != eqlrng.second; it++)
  {
    const Phase &phase = it->second;
    bool goodXcorr = xcorr.has(refEv.id, phase.stationId, phase.procInfo.type);

    if (phase.procInfo.type == Phase::Type::P) totP++;
    if (phase.procInfo.type == Phase::Type::S) totS++;

    // nothing to do if we dont't have good xcorr results of if the phase is
    // manual or from catalog
    if (!goodXcorr || phase.isManual ||
        (phase.procInfo.source == Phase::Source::CATALOG))
    {
      // remove thoretical phases without good cross-correlation results
      if (phase.procInfo.source == Phase::Source::THEORETICAL)
        phasesToBeRemoved.push_back(phase);
      continue;
    }

    const auto &pdata =
        xcorr.get(refEv.id, phase.stationId, phase.procInfo.type);

    //
    // set new phase time and uncertainty
    //
    Phase newPhase(phase);
    newPhase.time -= secToDur(pdata.mean_lag);
    newPhase.lowerUncertainty = pdata.mean_lag - pdata.min_lag;
    newPhase.upperUncertainty = pdata.max_lag - pdata.mean_lag;
    newPhase.procInfo.weight  = Catalog::computePickWeight(newPhase);
    newPhase.procInfo.source  = Phase::Source::XCORR;
    newPhase.type = strf("%cx", static_cast<char>(newPhase.procInfo.type));

    if (phase.procInfo.source == Phase::Source::THEORETICAL)
    {
      if (newPhase.procInfo.type == Phase::Type::P) newP++;
      if (newPhase.procInfo.type == Phase::Type::S) newS++;
    }

    newPhases.push_back(newPhase);
  }

  //
  // replace automatic/theoretical phases with those detected by means of
  // cross-correlation
  //
  for (const Phase &ph : phasesToBeRemoved)
  {
    if (ph.procInfo.type == Phase::Type::P) totP--;
    if (ph.procInfo.type == Phase::Type::S) totS--;
    catalog.removePhase(ph.eventId, ph.stationId, ph.procInfo.type);
  }

  for (Phase &ph : newPhases)
  {
    catalog.updatePhase(ph, true);
  }

  logDebug("Event %s total phases %u (%u P and %u S): created %u (%u P "
           "and %u S) from theoretical picks",
           string(refEv).c_str(), (totP + totS), totP, totS, (newP + newS),
           newP, newS);
}

void DD::printCounters() const
{
  unsigned skipped        = _counters.xcorr_skipped;
  unsigned performed      = _counters.xcorr_performed;
  unsigned performed_s    = _counters.xcorr_performed_s;
  unsigned performed_p    = performed - performed_s;
  unsigned perf_theo      = _counters.xcorr_performed_theo;
  unsigned perf_theo_s    = _counters.xcorr_performed_s_theo;
  unsigned perf_theo_p    = perf_theo - perf_theo_s;
  unsigned good_cc        = _counters.xcorr_good_cc;
  unsigned good_cc_s      = _counters.xcorr_good_cc_s;
  unsigned good_cc_p      = good_cc - good_cc_s;
  unsigned good_cc_theo   = _counters.xcorr_good_cc_theo;
  unsigned good_cc_s_theo = _counters.xcorr_good_cc_s_theo;
  unsigned good_cc_p_theo = good_cc_theo - good_cc_s_theo;

  logInfo("Cross-correlation performed %u (P phase %.f%%, S phase %.f%%), "
          "skipped %u (%.f%%)",
          performed, (performed_p * 100. / performed),
          (performed_s * 100. / performed), skipped,
          (skipped * 100. / (performed + skipped)));

  logInfo("Cross-correlation success (coefficient above threshold) %.f%% "
          "(%u/%u). Successful P %.f%% (%u/%u). Successful S %.f%% (%u/%u)",
          (good_cc * 100. / performed), good_cc, performed,
          (good_cc_p * 100. / performed_p), good_cc_p, performed_p,
          (good_cc_s * 100. / performed_s), good_cc_s, performed_s);

  if (perf_theo > 0)
  {
    unsigned perf_real      = performed - perf_theo,
             perf_real_s    = performed_s - perf_theo_s,
             perf_real_p    = perf_real - perf_real_s,
             good_cc_real   = good_cc - good_cc_theo,
             good_cc_s_real = good_cc_s - good_cc_s_theo,
             good_cc_p_real = good_cc_real - good_cc_s_real;

    logInfo(
        "xcorr on actual picks %u/%u (P %.f%%, S %.f%%) success %.f%% (%u/%u). "
        "Successful P %.f%% (%u/%u). Successful S %.f%% (%u/%u)",
        perf_real, performed, (perf_real_p * 100. / perf_real),
        (perf_real_s * 100. / perf_real), (good_cc_real * 100. / perf_real),
        good_cc_real, perf_real, (good_cc_p_real * 100. / perf_real_p),
        good_cc_p_real, perf_real_p, (good_cc_s_real * 100. / perf_real_s),
        good_cc_s_real, perf_real_s);

    logInfo("xcorr on theoretical picks %u/%u (P %.f%%, S %.f%%) success %.f%% "
            "(%u/%u). Successful P %.f%% (%u/%u). Successful S %.f%% (%u/%u)",
            perf_theo, performed, (perf_theo_p * 100. / perf_theo),
            (perf_theo_s * 100. / perf_theo), (good_cc_theo * 100. / perf_theo),
            good_cc_theo, perf_theo, (good_cc_p_theo * 100. / perf_theo_p),
            good_cc_p_theo, perf_theo_p, (good_cc_s_theo * 100. / perf_theo_s),
            good_cc_s_theo, perf_theo_s);
  }
}

TimeWindow DD::xcorrTimeWindowLong(const Phase &phase) const
{
  const auto xcorrCfg = _cfg.xcorr.at(phase.procInfo.type);
  TimeWindow tw       = xcorrTimeWindowShort(phase);
  tw.setStartTime(tw.startTime() - secToDur(xcorrCfg.maxDelay));
  tw.setEndTime(tw.endTime() + secToDur(xcorrCfg.maxDelay));
  return tw;
}

TimeWindow DD::xcorrTimeWindowShort(const Phase &phase) const
{
  const auto xcorrCfg = _cfg.xcorr.at(phase.procInfo.type);
  UTCTime::duration shortDuration =
      secToDur(xcorrCfg.endOffset - xcorrCfg.startOffset);
  UTCTime::duration shortTimeCorrection = secToDur(xcorrCfg.startOffset);
  return TimeWindow(phase.time + shortTimeCorrection, shortDuration);
}

bool DD::xcorrPhases(const Event &event1,
                     const Phase &phase1,
                     Waveform::Processor &ph1Cache,
                     const Event &event2,
                     const Phase &phase2,
                     Waveform::Processor &ph2Cache,
                     double &coeffOut,
                     double &lagOut)
{
  if (phase1.procInfo.type != phase2.procInfo.type)
  {
    logError("Internal logic error: trying to cross-correlate mismatching "
             "phases (%s and %s)",
             string(phase1).c_str(), string(phase2).c_str());
    _counters.xcorr_skipped++;
    return false;
  }

  //
  // Try to use the same channels for the cross-correlation. In case the two
  // phases differ, do not change the catalog phase channels.
  //
  const string channelCodeRoot1 = getBandAndInstrumentCodes(phase1.channelCode);
  const string channelCodeRoot2 = getBandAndInstrumentCodes(phase2.channelCode);

  if (channelCodeRoot1 != channelCodeRoot2)
  {
    bool channelsAreCompatible = false;
    for (const pair<string, string> &p : _cfg.compatibleChannels)
    {
      if ((p.first == channelCodeRoot1 && p.second == channelCodeRoot2) ||
          (p.second == channelCodeRoot1 && p.first == channelCodeRoot2))
      {
        channelsAreCompatible = true;
        break;
      }
    }

    if (!channelsAreCompatible)
    {
      logDebug("Skipping cross-correlation: incompatible channels %s and %s "
               "(%s and %s)",
               channelCodeRoot1.c_str(), channelCodeRoot2.c_str(),
               string(phase1).c_str(), string(phase2).c_str());
      _counters.xcorr_skipped++;
      return false;
    }
  }

  auto xcorrCfg  = _cfg.xcorr.at(phase1.procInfo.type);
  bool performed = false;
  bool goodCoeff = false;

  //
  // Try all registered components until we are able to perform
  // the cross-correlation
  //
  for (const string &component : xcorrCfg.components)
  {
    performed =
        xcorrPhasesOneComponent(event1, phase1, ph1Cache, event2, phase2,
                                ph2Cache, component, coeffOut, lagOut);
    coeffOut  = abs(coeffOut);
    goodCoeff = (performed && coeffOut >= xcorrCfg.minCoef);
    if (performed) break;
  }

  //
  // deal with counters
  //
  bool isS           = (phase1.procInfo.type == Phase::Type::S);
  bool isTheoretical = (phase1.procInfo.source == Phase::Source::XCORR ||
                        phase2.procInfo.source == Phase::Source::XCORR ||
                        phase1.procInfo.source == Phase::Source::THEORETICAL ||
                        phase2.procInfo.source == Phase::Source::THEORETICAL);

  if (performed)
  {
    _counters.xcorr_performed++;
    if (isTheoretical) _counters.xcorr_performed_theo++;
    if (isS)
    {
      _counters.xcorr_performed_s++;
      if (isTheoretical) _counters.xcorr_performed_s_theo++;
    }

    if (goodCoeff)
    {
      _counters.xcorr_good_cc++;
      if (isTheoretical) _counters.xcorr_good_cc_theo++;
      if (isS)
      {
        _counters.xcorr_good_cc_s++;
        if (isTheoretical) _counters.xcorr_good_cc_s_theo++;
      }
    }
  }
  else
    _counters.xcorr_skipped++;

  return goodCoeff;
}

bool DD::xcorrPhasesOneComponent(const Event &event1,
                                 const Phase &phase1,
                                 Waveform::Processor &ph1Cache,
                                 const Event &event2,
                                 const Phase &phase2,
                                 Waveform::Processor &ph2Cache,
                                 const string &component,
                                 double &coeffOut,
                                 double &lagOut)
{
  coeffOut = lagOut = 0;

  auto xcorrCfg = _cfg.xcorr.at(phase1.procInfo.type);

  TimeWindow tw1 = xcorrTimeWindowLong(phase1);
  TimeWindow tw2 = xcorrTimeWindowLong(phase2);

  // Load the long `tr1`, because we want to cache the long version. Then we'll
  // trim it.
  shared_ptr<const Trace> tr1 =
      getWaveform(ph1Cache, tw1, event1, phase1, component);
  if (!tr1)
  {
    return false;
  }

  // Load the long `tr2`, because we want to cache the long version. Then we'll
  // trim it.
  shared_ptr<const Trace> tr2 =
      getWaveform(ph2Cache, tw2, event2, phase2, component);
  if (!tr2)
  {
    return false;
  }

  // Trust the manual pick on `phase2`: keep `tr2` short and cross-correlate it
  // with the larger `tr1` window.
  double xcorr_coeff = 0, xcorr_lag = 0;

  if (phase2.isManual || (!phase1.isManual && !phase2.isManual))
  {
    // Trim `tr2` to shorter length; we want to cross-correlate the short one
    // with the long one.
    Trace tr2Short(*tr2);
    TimeWindow tw2Short = xcorrTimeWindowShort(phase2);
    if (!tr2Short.slice(tw2Short))
    {
      logDebug("Skipping cross-correlation: cannot trim phase2 waveform "
               "for phase pair phase1='%s', phase2='%s'",
               string(phase1).c_str(), string(phase2).c_str());
      return false;
    }

    if (!xcorr(*tr1, tr2Short, xcorrCfg.maxDelay, true, xcorr_lag, xcorr_coeff))
    {
      return false;
    }
  }

  // Trust the manual pick on `phase1`: keep `tr1` short and cross-correlate it
  // with a larger `tr2` window.
  double xcorr_coeff2 = 0, xcorr_lag2 = 0;

  if (phase1.isManual || (!phase1.isManual && !phase2.isManual))
  {
    // Trim `tr1` to shorter length; we want to cross-correlate the short with
    // the long one.
    Trace tr1Short(*tr1);
    TimeWindow tw1Short = xcorrTimeWindowShort(phase1);
    if (!tr1Short.slice(tw1Short))
    {
      logDebug("Skipping cross-correlation: cannot trim phase1 waveform "
               "for phase pair phase1='%s', phase2='%s'",
               string(phase1).c_str(), string(phase2).c_str());
      return false;
    }

    if (!xcorr(tr1Short, *tr2, xcorrCfg.maxDelay, true, xcorr_lag2,
               xcorr_coeff2))
    {
      return false;
    }
  }

  if (std::abs(xcorr_coeff2) > std::abs(xcorr_coeff))
  {
    // swap
    xcorr_coeff = xcorr_coeff2;
    xcorr_lag   = xcorr_lag2;
  }

  coeffOut = xcorr_coeff;
  lagOut   = xcorr_lag;

  return true;
}

/*
 * Compute cross-correlation between two traces centered around their respective
 * picks. The cross-correlation will be performed with the short trace against
 * the longest trace starting from the longest trace middle point minus
 * 'maxDelay' until middle point pluse 'maxDelay'. `delayOut` will store the
 * shift in seconds (positive or negative) from the longest trace middle point
 * at which there is the highest (absolute value) correlation coefficient,
 * stored in 'coeffOut'
 */
bool DD::xcorr(const Trace &tr1,
               const Trace &tr2,
               double maxDelay,
               bool qualityCheck,
               double &delayOut,
               double &coeffOut)
{
  if (tr1.samplingFrequency() != tr2.samplingFrequency())
  {
    logInfo("Skipping cross-correlation: traces have different sampling freq "
            "(%f!=%f)",
            tr1.samplingFrequency(), tr2.samplingFrequency());
    return false;
  }
  const double freq = tr1.samplingFrequency();

  // check longest/shortest trace
  const bool swap        = tr1.sampleCount() > tr2.sampleCount();
  const Trace &trShorter = swap ? tr2 : tr1;
  const Trace &trLonger  = swap ? tr1 : tr2;

  const double *dataS = trShorter.data();
  const double *dataL = trLonger.data();
  const int sizeS     = trShorter.sampleCount();
  const int sizeL     = trLonger.sampleCount();

  // force to cross-correlate withing data boundaries
  int availableData = (sizeL - sizeS) / 2;
  int maxDelaySmps  = maxDelay * freq;
  if (maxDelaySmps > availableData) maxDelaySmps = availableData;

  crossCorrelation(dataS, sizeS, (dataL + availableData - maxDelaySmps),
                   (sizeS + maxDelaySmps * 2), qualityCheck, delayOut,
                   coeffOut);

  if (!std::isfinite(coeffOut))
  {
    coeffOut = 0;
    delayOut = 0.;
  }
  else
  {
    delayOut -= maxDelaySmps; // the reference is the middle of the long trace
    delayOut /= freq;         // samples to secs
    if (swap) delayOut = -delayOut;
  }
  return true;
}

shared_ptr<const Trace> DD::getWaveform(Waveform::Processor &wfProc,
                                        const TimeWindow &tw,
                                        const Event &ev,
                                        const Phase &ph,
                                        const string &component)
{
  const Station &sta = _bgCat.getStations().at(ph.stationId);

  Catalog::Phase phCopy(ph);
  Transform trans;
  if (component == "T")
  {
    trans = Transform::TRANSVERSAL;
  }
  else if (component == "R")
  {
    trans = Transform::RADIAL;
  }
  else if (component == "L")
  {
    trans = Transform::L2;
  }
  else
  {
    trans              = Transform::NONE;
    phCopy.channelCode = getBandAndInstrumentCodes(ph.channelCode) + component;
  }

  return wfProc.get(tw, phCopy, ev, sta, _cfg.wfFilter.filterStr,
                    _cfg.wfFilter.resampleFreq, trans);
}

namespace {

struct XCorrEvalStats
{
  unsigned total  = 0;
  unsigned goodCC = 0;
  vector<double> ccCoeff;
  vector<double> ccCount;
  vector<double> timeDiff;

  void addBadCC() { total++; }

  void addGoodCC(double ccCoeff, unsigned ccCount, double timeDiff)
  {
    this->total++;
    this->goodCC++;
    this->ccCoeff.push_back(ccCoeff);
    this->ccCount.push_back(ccCount);
    this->timeDiff.push_back(timeDiff);
  }

  XCorrEvalStats &operator+=(XCorrEvalStats const &rhs) &
  {
    total += rhs.total;
    goodCC += rhs.goodCC;
    ccCoeff.insert(ccCoeff.end(), rhs.ccCoeff.begin(), rhs.ccCoeff.end());
    ccCount.insert(ccCount.end(), rhs.ccCount.begin(), rhs.ccCount.end());
    timeDiff.insert(timeDiff.end(), rhs.timeDiff.begin(), rhs.timeDiff.end());
    return *this;
  }

  friend XCorrEvalStats operator+(XCorrEvalStats lhs, XCorrEvalStats const &rhs)
  {
    lhs += rhs;
    return lhs;
  }

  string describeShort() const
  {
    double meanCoeff    = computeMean(ccCoeff);
    double meanCoeffMAD = computeMeanAbsoluteDeviation(ccCoeff, meanCoeff);
    double meanCount    = computeMean(ccCount);
    double meanCountMAD = computeMeanAbsoluteDeviation(ccCount, meanCount);
    string log        = strf("#pha %6d pha good CC %3.f%% coeff %.2f (+/-%.2f) "
                      "goodCC/ph %4.1f (+/-%.1f)",
                      total, (goodCC * 100. / total), meanCoeff, meanCoeffMAD,
                      meanCount, meanCountMAD);
    double meanDev    = computeMean(timeDiff);
    double meanDevMAD = computeMeanAbsoluteDeviation(timeDiff, meanDev);
    log += strf(" time-diff [msec] %3.f (+/-%.f)", meanDev * 1000,
                meanDevMAD * 1000);
    return log;
  }

  string describe() const
  {
    double meanCoeff    = computeMean(ccCoeff);
    double meanCoeffMAD = computeMeanAbsoluteDeviation(ccCoeff, meanCoeff);
    double meanCount    = computeMean(ccCount);
    double meanCountMAD = computeMeanAbsoluteDeviation(ccCount, meanCount);
    string log        = strf("%9d %5.f%% % 4.2f (%4.2f)   %4.1f (%4.1f)", total,
                      (goodCC * 100. / total), meanCoeff, meanCoeffMAD,
                      meanCount, meanCountMAD);
    double meanDev    = computeMean(timeDiff);
    double meanDevMAD = computeMeanAbsoluteDeviation(timeDiff, meanDev);
    log += strf("%8.f (%3.f)", meanDev * 1000, meanDevMAD * 1000);
    return log;
  }
};
} // namespace

void DD::evalXCorr(const ClusteringOptions &clustOpt, bool theoretical)
{
  XCorrEvalStats totalStats, pPhaseStats, sPhaseStats;
  map<string, XCorrEvalStats> statsByStation;      // key station id
  map<int, XCorrEvalStats> statsByInterEvDistance; // key distance
  map<int, XCorrEvalStats> statsByStaDistance;     // key distance
  const double EV_DIST_STEP  = 0.1;                // km
  const double STA_DIST_STEP = 3;                  // km

  auto printStats = [&](string title) {
    string log = title + "\n";
    log += strf("Cumulative stats: %s\n", totalStats.describeShort().c_str());
    log += strf("Cumulative stats P ph: %s\n",
                pPhaseStats.describeShort().c_str());
    log += strf("Cumulative stats S ph: %s\n",
                sPhaseStats.describeShort().c_str());

    log += strf(
        "Cross-correlated phases by inter-event distance in %.2f km step\n",
        EV_DIST_STEP);
    log += strf(" EvDist [km]  #Phases GoodCC AvgCoeff(+/-) "
                "GoodCC/Ph(+/-) time-diff[msec] (+/-)\n");
    for (const auto &kv : statsByInterEvDistance)
    {
      log += strf("%5.2f-%-5.2f %s\n", kv.first * EV_DIST_STEP,
                  (kv.first + 1) * EV_DIST_STEP, kv.second.describe().c_str());
    }

    log += strf("Cross-correlated phases by event to station distance in "
                "%.2f km step\n",
                STA_DIST_STEP);
    log += strf("StaDist [km]  #Phases GoodCC AvgCoeff(+/-) "
                "GoodCC/Ph(+/-) time-diff[msec] (+/-)\n");
    for (const auto &kv : statsByStaDistance)
    {
      log += strf("%3d-%-3d     %s\n", int(kv.first * STA_DIST_STEP),
                  int((kv.first + 1) * STA_DIST_STEP),
                  kv.second.describe().c_str());
    }

    log += strf("Cross-correlations by station\n");
    log += strf("Station       #Phases GoodCC AvgCoeff(+/-) "
                "GoodCC/Ph(+/-) time-diff[msec] (+/-)\n");
    for (const auto &kv : statsByStation)
    {
      log += strf("%-12s %s\n", kv.first.c_str(), kv.second.describe().c_str());
    }
    logInfo("%s", log.c_str());
  };

  WfCounters wfcount{0};
  _counters.reset();
  int loop = 0;

  for (const auto &kv : _bgCat.getEvents())
  {
    const Event &event = kv.second;

    // find the neighbouring events
    unique_ptr<Neighbours> neighbours;
    try
    {
      neighbours = selectNeighbouringEvents(
          _bgCat, event, _bgCat, clustOpt.minWeight, clustOpt.minESdist,
          clustOpt.maxESdist, clustOpt.minEStoIEratio, clustOpt.minDTperEvt,
          clustOpt.maxDTperEvt, clustOpt.minNumNeigh, clustOpt.maxNumNeigh,
          clustOpt.numEllipsoids, clustOpt.maxEllipsoidSize, false);
    }
    catch (...)
    {
      continue;
    }

    unique_ptr<Catalog> catalog;

    if (theoretical)
    {
      // create theoretical phases for this event instead of fetching its
      // phases from the catalog
      catalog = neighbours->toCatalog(_bgCat, false);
      addMissingEventPhases(event, *catalog, _bgCat, *neighbours);
    }
    else
    {
      catalog = neighbours->toCatalog(_bgCat, true);
    }

    // Cross-correlate every neighbour phase with its corresponding event
    // theoretical phase.
    XCorrCache xcorr;
    buildXcorrDiffTTimePairs(*catalog, *neighbours, event,
                             clustOpt.xcorrMaxEvStaDist,
                             clustOpt.xcorrMaxInterEvDist, xcorr);

    // Update theoretical and automatic phase pick time and uncertainties based
    // on cross-correlation results. Drop theoretical phases wihout any good
    // cross-correlation result.
    if (theoretical)
    {
      fixPhases(*catalog, event, xcorr);
    }

    //
    // Compare the detected phases with the actual event phases (manual or
    // automatic).
    //
    XCorrEvalStats evStats;

    for (const auto &kv : neighbours->allPhases())
      for (Phase::Type phaseType : kv.second)
      {
        //
        //  collect stats by event, station, station distance
        //
        const string stationId = kv.first;
        const Phase &catalogPhase =
            _bgCat.searchPhase(event.id, stationId, phaseType)->second;

        XCorrEvalStats phStaStats;
        double phaseTimeDiff = 0;

        if (xcorr.has(event.id, stationId, phaseType))
        {
          const auto &xentry = xcorr.get(event.id, stationId, phaseType);
          if (theoretical)
          {
            const Phase &detectedPhase =
                catalog->searchPhase(event.id, stationId, phaseType)->second;
            phaseTimeDiff = durToSec(catalogPhase.time - detectedPhase.time);
          }
          else
          {
            phaseTimeDiff = xentry.mean_lag;
          }
          phStaStats.addGoodCC(xentry.mean_coeff, xentry.ccCount,
                               phaseTimeDiff);
        }
        else
        {
          phStaStats.addBadCC();
        }

        evStats += phStaStats;
        totalStats += phStaStats;
        if (phaseType == Phase::Type::P) pPhaseStats += phStaStats;
        if (phaseType == Phase::Type::S) sPhaseStats += phStaStats;
        statsByStation[catalogPhase.stationId] += phStaStats;

        const Station &station =
            _bgCat.getStations().at(catalogPhase.stationId);
        double stationDistance = computeDistance(event, station);
        statsByStaDistance[int(stationDistance / STA_DIST_STEP)] += phStaStats;

        //
        //  collect stats by inter-event distance
        //
        map<unsigned, XCorrEvalStats> tmpStatsByInterEvDistance;

        for (unsigned neighEvId : neighbours->ids)
        {
          if (neighbours->has(neighEvId, stationId, phaseType))
          {
            const Event &neighbEv  = catalog->getEvents().at(neighEvId);
            double interEvDistance = computeDistance(event, neighbEv);
            XCorrEvalStats &interEvDistStats =
                tmpStatsByInterEvDistance[int(interEvDistance / EV_DIST_STEP)];
            interEvDistStats.total = 1;
            if (xcorr.has(event.id, neighEvId, stationId, phaseType))
            {
              const auto &xpi =
                  xcorr.get(event.id, neighEvId, stationId, phaseType);
              interEvDistStats.goodCC = 1;
              interEvDistStats.ccCount.push_back(1);
              interEvDistStats.ccCoeff.push_back(xpi.coeff);
              if (theoretical)
              {
                const auto &xentry = xcorr.get(event.id, stationId, phaseType);
                double timeDiff = phaseTimeDiff - (xentry.mean_lag - xpi.lag);
                interEvDistStats.timeDiff.push_back(timeDiff);
              }
              else
              {
                interEvDistStats.timeDiff.push_back(xpi.lag);
              }
            }
          }
        }

        for (auto &kv : tmpStatsByInterEvDistance)
        {
          const double interEvDistanceBucket = kv.first;
          XCorrEvalStats &newStats           = kv.second;
          if (newStats.goodCC > 0)
          {
            newStats.ccCount  = {std::accumulate(newStats.ccCount.begin(),
                                                newStats.ccCount.end(), 0.)};
            newStats.ccCoeff  = {computeMean(newStats.ccCoeff)};
            newStats.timeDiff = {computeMean(newStats.timeDiff)};
          }
          statsByInterEvDistance[interEvDistanceBucket] += newStats;
        }
      }

    logInfo("Event %-5s mag %3.1f %s", string(event).c_str(), event.magnitude,
            evStats.describeShort().c_str());

    if (++loop % 100 == 0)
    {
      printStats("<PROGRESSIVE STATS>");
    }
  }

  printStats("<FINAL STATS>");
  wfcount.update(_wfAccess.loader, _wfAccess.diskCache, _wfAccess.snrFilter);
  logInfo("Catalog waveform data: waveforms downloaded %u, not available "
          "%u, loaded from disk cache %u. Waveforms with SNR too low %u",
          wfcount.downloaded, wfcount.no_avail, wfcount.disk_cached,
          wfcount.snr_low);
  printCounters();
}

} // namespace HDD
