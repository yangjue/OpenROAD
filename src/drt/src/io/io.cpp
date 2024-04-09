/* Authors: Lutong Wang and Bangqi Xu */
/*
 * Copyright (c) 2019, The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "io/io.h"

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>

#include "db/tech/frConstraint.h"
#include "frProfileTask.h"
#include "frRTree.h"
#include "global.h"
#include "odb/db.h"
#include "odb/dbShape.h"
#include "odb/dbWireCodec.h"
#include "triton_route/TritonRoute.h"
#include "utl/Logger.h"

namespace drt {

io::Parser::Parser(odb::dbDatabase* dbIn, frDesign* designIn, Logger* loggerIn)
    : db_(dbIn),
      design_(designIn),
      tech_(design_->getTech()),
      logger_(loggerIn),
      tmpBlock_(nullptr),
      readLayerCnt_(0),
      masterSliceLayer_(nullptr),
      numMasters_(0),
      numInsts_(0),
      numTerms_(0),
      numNets_(0),
      numBlockages_(0)
{
}

void io::Parser::setDieArea(odb::dbBlock* block)
{
  std::vector<frBoundary> bounds;
  frBoundary bound;
  std::vector<Point> points;
  odb::Rect box = block->getDieArea();
  points.emplace_back(box.xMin(), box.yMin());
  points.emplace_back(box.xMax(), box.yMax());
  points.emplace_back(box.xMax(), box.yMin());
  points.emplace_back(box.xMin(), box.yMax());
  bound.setPoints(points);
  bounds.push_back(bound);
  tmpBlock_->setDBUPerUU(block->getDbUnitsPerMicron());
  tmpBlock_->setBoundaries(bounds);
}

void io::Parser::setTracks(odb::dbBlock* block)
{
  auto tracks = block->getTrackGrids();
  for (auto track : tracks) {
    if (tech_->name2layer_.find(track->getTechLayer()->getName())
        == tech_->name2layer_.end()) {
      logger_->error(
          DRT, 94, "Cannot find layer: {}.", track->getTechLayer()->getName());
    }
    int xPatternSize = track->getNumGridPatternsX();
    int yPatternSize = track->getNumGridPatternsY();
    for (int i = 0; i < xPatternSize; i++) {
      std::unique_ptr<frTrackPattern> tmpTrackPattern
          = std::make_unique<frTrackPattern>();
      tmpTrackPattern->setLayerNum(
          tech_->name2layer_.at(track->getTechLayer()->getName())
              ->getLayerNum());
      tmpTrackPattern->setHorizontal(true);
      int startCoord, numTracks, step;
      track->getGridPatternX(i, startCoord, numTracks, step);
      tmpTrackPattern->setStartCoord(startCoord);
      tmpTrackPattern->setNumTracks(numTracks);
      tmpTrackPattern->setTrackSpacing(step);
      tmpBlock_->trackPatterns_.at(tmpTrackPattern->getLayerNum())
          .push_back(std::move(tmpTrackPattern));
    }
    for (int i = 0; i < yPatternSize; i++) {
      std::unique_ptr<frTrackPattern> tmpTrackPattern
          = std::make_unique<frTrackPattern>();
      tmpTrackPattern->setLayerNum(
          tech_->name2layer_.at(track->getTechLayer()->getName())
              ->getLayerNum());
      tmpTrackPattern->setHorizontal(false);
      int startCoord, numTracks, step;
      track->getGridPatternY(i, startCoord, numTracks, step);
      tmpTrackPattern->setStartCoord(startCoord);
      tmpTrackPattern->setNumTracks(numTracks);
      tmpTrackPattern->setTrackSpacing(step);
      tmpBlock_->trackPatterns_.at(tmpTrackPattern->getLayerNum())
          .push_back(std::move(tmpTrackPattern));
    }
  }
}

void io::Parser::setInsts(odb::dbBlock* block)
{
  for (auto inst : block->getInsts()) {
    if (design_->name2master_.find(inst->getMaster()->getName())
        == design_->name2master_.end()) {
      logger_->error(
          DRT, 95, "Library cell {} not found.", inst->getMaster()->getName());
    }
    if (tmpBlock_->name2inst_.find(inst->getName())
        != tmpBlock_->name2inst_.end()) {
      logger_->error(DRT, 96, "Same cell name: {}.", inst->getName());
    }
    frMaster* master = design_->name2master_.at(inst->getMaster()->getName());
    auto uInst = std::make_unique<frInst>(inst->getName(), master);
    auto tmpInst = uInst.get();
    tmpInst->setId(numInsts_);
    numInsts_++;

    int x, y;
    inst->getLocation(x, y);
    tmpInst->setOrigin(Point(x, y));
    tmpInst->setOrient(inst->getOrient());
    int numInstTerms = 0;
    tmpInst->setPinAccessIdx(inst->getPinAccessIdx());
    for (auto& uTerm : tmpInst->getMaster()->getTerms()) {
      auto term = uTerm.get();
      std::unique_ptr<frInstTerm> instTerm
          = std::make_unique<frInstTerm>(tmpInst, term);
      instTerm->setId(numTerms_++);
      instTerm->setIndexInOwner(numInstTerms++);
      int pinCnt = term->getPins().size();
      instTerm->setAPSize(pinCnt);
      tmpInst->addInstTerm(std::move(instTerm));
    }
    for (auto& uBlk : tmpInst->getMaster()->getBlockages()) {
      auto blk = uBlk.get();
      std::unique_ptr<frInstBlockage> instBlk
          = std::make_unique<frInstBlockage>(tmpInst, blk);
      instBlk->setId(numBlockages_);
      numBlockages_++;
      tmpInst->addInstBlockage(std::move(instBlk));
    }
    tmpBlock_->addInst(std::move(uInst));
  }
}

void io::Parser::setObstructions(odb::dbBlock* block)
{
  for (auto blockage : block->getObstructions()) {
    std::string layerName = blockage->getBBox()->getTechLayer()->getName();
    if (tech_->name2layer_.find(layerName) == tech_->name2layer_.end()) {
      logger_->warn(
          DRT, 282, "Skipping blockage. Cannot find layer {}.", layerName);
      continue;
    }
    frLayerNum layerNum = tech_->name2layer_[layerName]->getLayerNum();
    auto blkIn = std::make_unique<frBlockage>();
    blkIn->setId(numBlockages_);
    numBlockages_++;
    auto pinIn = std::make_unique<frBPin>();
    pinIn->setId(0);
    frCoord xl = blockage->getBBox()->xMin();
    frCoord yl = blockage->getBBox()->yMin();
    frCoord xh = blockage->getBBox()->xMax();
    frCoord yh = blockage->getBBox()->yMax();
    // pinFig
    std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
    pinFig->setBBox(Rect(xl, yl, xh, yh));
    pinFig->addToPin(pinIn.get());
    pinFig->setLayerNum(layerNum);
    // pinFig completed
    std::unique_ptr<frPinFig> uptr(std::move(pinFig));
    pinIn->addPinFig(std::move(uptr));

    blkIn->setPin(std::move(pinIn));
    tmpBlock_->addBlockage(std::move(blkIn));
  }
}

void io::Parser::setVias(odb::dbBlock* block)
{
  for (auto via : block->getVias()) {
    if (via->getViaGenerateRule() != nullptr && via->hasParams()) {
      const odb::dbViaParams params = via->getViaParams();
      frLayerNum cutLayerNum = 0;
      frLayerNum botLayerNum = 0;
      frLayerNum topLayerNum = 0;

      if (tech_->name2layer_.find(params.getCutLayer()->getName())
          == tech_->name2layer_.end()) {
        logger_->error(DRT,
                       97,
                       "Cannot find cut layer {}.",
                       params.getCutLayer()->getName());
      } else {
        cutLayerNum = tech_->name2layer_.find(params.getCutLayer()->getName())
                          ->second->getLayerNum();
      }
      if (tech_->name2layer_.find(params.getBottomLayer()->getName())
          == tech_->name2layer_.end()) {
        logger_->error(DRT,
                       98,
                       "Cannot find bottom layer {}.",
                       params.getBottomLayer()->getName());
      } else {
        if (params.getBottomLayer()->getType()
            == dbTechLayerType::MASTERSLICE) {
          logger_->warn(DRT,
                        190,
                        "Via {} with MASTERSLICE layer {} will be ignored.",
                        params.getBottomLayer()->getName(),
                        via->getName());
          continue;
        }
        botLayerNum
            = tech_->name2layer_.find(params.getBottomLayer()->getName())
                  ->second->getLayerNum();
      }

      if (tech_->name2layer_.find(params.getTopLayer()->getName())
          == tech_->name2layer_.end()) {
        logger_->error(DRT,
                       99,
                       "Cannot find top layer {}.",
                       params.getTopLayer()->getName());
      } else {
        if (params.getTopLayer()->getType() == dbTechLayerType::MASTERSLICE) {
          logger_->warn(DRT,
                        191,
                        "Via {} with MASTERSLICE layer {} will be ignored.",
                        params.getTopLayer()->getName(),
                        via->getName());
          continue;
        }
        topLayerNum = tech_->name2layer_.find(params.getTopLayer()->getName())
                          ->second->getLayerNum();
      }
      int xSize = params.getXCutSize();
      int ySize = params.getYCutSize();
      int xCutSpacing = params.getXCutSpacing();
      int yCutSpacing = params.getYCutSpacing();
      int xOffset = params.getXOrigin();
      int yOffset = params.getYOrigin();
      int xTopEnc = params.getXTopEnclosure();
      int yTopEnc = params.getYTopEnclosure();
      int xBotEnc = params.getXBottomEnclosure();
      int yBotEnc = params.getYBottomEnclosure();
      int xTopOffset = params.getXTopOffset();
      int yTopOffset = params.getYTopOffset();
      int xBotOffset = params.getXBottomOffset();
      int yBotOffset = params.getYBottomOffset();

      frCoord currX = 0;
      frCoord currY = 0;
      std::vector<std::unique_ptr<frShape>> cutFigs;
      for (int i = 0; i < params.getNumCutRows(); i++) {
        currX = 0;
        for (int j = 0; j < params.getNumCutCols(); j++) {
          auto rect = std::make_unique<frRect>();
          Rect tmpBox(currX, currY, currX + xSize, currY + ySize);
          rect->setBBox(tmpBox);
          rect->setLayerNum(cutLayerNum);
          cutFigs.push_back(std::move(rect));
          currX += xSize + xCutSpacing;
        }
        currY += ySize + yCutSpacing;
      }
      currX -= xCutSpacing;  // max cut X
      currY -= yCutSpacing;  // max cut Y
      dbTransform cutXform(Point(-currX / 2 + xOffset, -currY / 2 + yOffset));
      for (auto& uShape : cutFigs) {
        auto rect = static_cast<frRect*>(uShape.get());
        rect->move(cutXform);
      }
      std::unique_ptr<frShape> uBotFig = std::make_unique<frRect>();
      auto botFig = static_cast<frRect*>(uBotFig.get());
      std::unique_ptr<frShape> uTopFig = std::make_unique<frRect>();
      auto topFig = static_cast<frRect*>(uTopFig.get());

      Rect botBox(0 - xBotEnc, 0 - yBotEnc, currX + xBotEnc, currY + yBotEnc);
      Rect topBox(0 - xTopEnc, 0 - yTopEnc, currX + xTopEnc, currY + yTopEnc);

      dbTransform botXform(Point(-currX / 2 + xOffset + xBotOffset,
                                 -currY / 2 + yOffset + yBotOffset));
      dbTransform topXform(Point(-currX / 2 + xOffset + xTopOffset,
                                 -currY / 2 + yOffset + yTopOffset));
      botXform.apply(botBox);
      topXform.apply(topBox);

      botFig->setBBox(botBox);
      topFig->setBBox(topBox);
      botFig->setLayerNum(botLayerNum);
      topFig->setLayerNum(topLayerNum);

      auto viaDef = std::make_unique<frViaDef>(via->getName());
      viaDef->addLayer1Fig(std::move(uBotFig));
      viaDef->addLayer2Fig(std::move(uTopFig));
      auto cutLayer = tech_->getLayer(cutLayerNum);
      for (auto& uShape : cutFigs) {
        viaDef->addCutFig(std::move(uShape));
      }
      int cutClassIdx = -1;
      frLef58CutClass* cutClass = nullptr;
      for (auto& cutFig : viaDef->getCutFigs()) {
        Rect box = cutFig->getBBox();
        auto width = box.minDXDY();
        auto length = box.maxDXDY();
        cutClassIdx = cutLayer->getCutClassIdx(width, length);
        if (cutClassIdx != -1) {
          cutClass = cutLayer->getCutClass(cutClassIdx);
          break;
        }
      }
      if (cutClass) {
        viaDef->setCutClass(cutClass);
        viaDef->setCutClassIdx(cutClassIdx);
      }
      if (!tech_->addVia(std::move(viaDef))) {
        logger_->error(
            utl::DRT, 337, "Duplicated via definition for {}", via->getName());
      }
    } else {
      std::map<frLayerNum, std::set<odb::dbBox*>> lNum2Int;
      for (auto box : via->getBoxes()) {
        if (tech_->name2layer_.find(box->getTechLayer()->getName())
            == tech_->name2layer_.end()) {
          return;
        }
        auto layerNum = tech_->name2layer_.at(box->getTechLayer()->getName())
                            ->getLayerNum();
        lNum2Int[layerNum].insert(box);
      }
      if ((int) lNum2Int.size() != 3) {
        logger_->error(DRT, 100, "Unsupported via: {}.", via->getName());
      }
      if (lNum2Int.begin()->first + 2 != (--lNum2Int.end())->first) {
        logger_->error(
            DRT, 101, "Non-consecutive layers for via: {}.", via->getName());
      }
      auto viaDef = std::make_unique<frViaDef>(via->getName());
      int cnt = 0;
      for (auto& [layerNum, boxes] : lNum2Int) {
        for (auto box : boxes) {
          std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
          pinFig->setBBox(
              Rect(box->xMin(), box->yMin(), box->xMax(), box->yMax()));
          pinFig->setLayerNum(layerNum);
          switch (cnt) {
            case 0:
              viaDef->addLayer1Fig(std::move(pinFig));
              break;
            case 1:
              viaDef->addCutFig(std::move(pinFig));
              break;
            default:
              viaDef->addLayer2Fig(std::move(pinFig));
              break;
          }
        }
        cnt++;
      }
      if (via->isDefault()) {
        viaDef->setDefault(true);
      }
      if (!tech_->addVia(std::move(viaDef))) {
        logger_->error(
            utl::DRT, 338, "Duplicated via definition for {}", via->getName());
      }
    }
  }
}

void io::Parser::createNDR(odb::dbTechNonDefaultRule* ndr)
{
  if (design_->tech_->getNondefaultRule(ndr->getName())) {
    logger_->warn(DRT,
                  256,
                  "Skipping NDR {} because another rule with the same name "
                  "already exists.",
                  ndr->getName());
    return;
  }
  frNonDefaultRule* fnd;
  std::unique_ptr<frNonDefaultRule> ptnd;
  int z;
  ptnd = std::make_unique<frNonDefaultRule>();
  fnd = ptnd.get();
  design_->tech_->addNDR(std::move(ptnd));
  fnd->setName(ndr->getName().data());
  fnd->setHardSpacing(ndr->getHardSpacing());
  std::vector<odb::dbTechLayerRule*> lr;
  ndr->getLayerRules(lr);
  for (auto& l : lr) {
    z = design_->tech_->getLayer(l->getLayer()->getName())->getLayerNum() / 2
        - 1;
    fnd->setWidth(l->getWidth(), z);
    fnd->setSpacing(l->getSpacing(), z);
    fnd->setWireExtension(l->getWireExtension(), z);
  }
  std::vector<odb::dbTechVia*> vias;
  ndr->getUseVias(vias);
  for (auto via : vias) {
    fnd->addVia(design_->getTech()->getVia(via->getName()),
                via->getBottomLayer()->getNumber() / 2);
  }
  std::vector<odb::dbTechViaGenerateRule*> viaRules;
  ndr->getUseViaRules(viaRules);
  z = std::numeric_limits<int>::max();
  for (auto via : viaRules) {
    for (int i = 0; i < (int) via->getViaLayerRuleCount(); i++) {
      if (via->getViaLayerRule(i)->getLayer()->getType()
          == odb::dbTechLayerType::CUT) {
        continue;
      }
      if (via->getViaLayerRule(i)->getLayer()->getNumber() / 2 < z) {
        z = via->getViaLayerRule(i)->getLayer()->getNumber() / 2;
      }
    }
    fnd->addViaRule(design_->getTech()->getViaRule(via->getName()), z);
  }
}

void io::Parser::setNDRs(odb::dbDatabase* db)
{
  for (auto ndr : db->getTech()->getNonDefaultRules()) {
    createNDR(ndr);
  }
  for (auto ndr : db->getChip()->getBlock()->getNonDefaultRules()) {
    createNDR(ndr);
  }
  for (auto& layer : design_->getTech()->getLayers()) {
    if (layer->getType() != dbTechLayerType::ROUTING) {
      continue;
    }
    MTSAFEDIST = std::max(MTSAFEDIST,
                          design_->getTech()->getMaxNondefaultSpacing(
                              layer->getLayerNum() / 2 - 1));
  }
}

void io::Parser::getSBoxCoords(odb::dbSBox* box,
                               frCoord& beginX,
                               frCoord& beginY,
                               frCoord& endX,
                               frCoord& endY,
                               frCoord& width)
{
  int x1 = box->xMin();
  int y1 = box->yMin();
  int x2 = box->xMax();
  int y2 = box->yMax();
  uint dx = box->getDX();
  uint dy = box->getDY();
  uint w;
  switch (box->getDirection()) {
    case odb::dbSBox::UNDEFINED: {
      bool dx_even = ((dx & 1) == 0);
      bool dy_even = ((dy & 1) == 0);
      if (dx_even && dy_even) {
        if (dy < dx) {
          w = dy;
          uint dw = dy >> 1;
          y1 += dw;
          y2 -= dw;
          assert(y1 == y2);
        } else {
          w = dx;
          uint dw = dx >> 1;
          x1 += dw;
          x2 -= dw;
          assert(x1 == x2);
        }
      } else if (dx_even) {
        w = dx;
        uint dw = dx >> 1;
        x1 += dw;
        x2 -= dw;
        assert(x1 == x2);
      } else if (dy_even) {
        w = dy;
        uint dw = dy >> 1;
        y1 += dw;
        y2 -= dw;
        assert(y1 == y2);
      } else {
        logger_->error(DRT, 102, "Odd dimension in both directions.");
      }
      break;
    }
    case odb::dbSBox::HORIZONTAL: {
      w = dy;
      uint dw = dy >> 1;
      y1 += dw;
      y2 -= dw;
      assert(y1 == y2);
      break;
    }
    case odb::dbSBox::VERTICAL: {
      w = dx;
      uint dw = dx >> 1;
      x1 += dw;
      x2 -= dw;
      assert(x1 == x2);
      break;
    }
    case odb::dbSBox::OCTILINEAR: {
      odb::Oct oct = box->getOct();
      x1 = oct.getCenterLow().getX();
      y1 = oct.getCenterLow().getY();
      x2 = oct.getCenterHigh().getX();
      y2 = oct.getCenterHigh().getY();
      w = oct.getWidth();
      break;
    }
    default:
      logger_->error(DRT, 103, "Unknown direction.");
      break;
  }
  beginX = x1;
  endX = x2;
  beginY = y1;
  endY = y2;
  width = w;
}

void io::Parser::setNets(odb::dbBlock* block)
{
  for (auto net : block->getNets()) {
    bool is_special = net->isSpecial();
    if (!is_special && net->getSigType().isSupply()) {
      logger_->error(DRT,
                     305,
                     "Net {} of signal type {} is not routable by TritonRoute. "
                     "Move to special nets.",
                     net->getName(),
                     net->getSigType().getString());
    }
    std::unique_ptr<frNet> uNetIn = std::make_unique<frNet>(net->getName());
    auto netIn = uNetIn.get();
    if (net->getNonDefaultRule()) {
      uNetIn->updateNondefaultRule(design_->getTech()->getNondefaultRule(
          net->getNonDefaultRule()->getName()));
    }
    if (net->getSigType() == dbSigType::CLOCK) {
      uNetIn->updateIsClock(true);
    }
    if (is_special) {
      uNetIn->setIsSpecial(true);
    }
    netIn->setId(numNets_);
    numNets_++;
    for (auto term : net->getBTerms()) {
      if (term->getSigType().isSupply() && !net->getSigType().isSupply()) {
        logger_->error(DRT,
                       306,
                       "Net {} of signal type {} cannot be connected to bterm "
                       "{} with signal type {}",
                       net->getName(),
                       net->getSigType().getString(),
                       term->getName(),
                       term->getSigType().getString());
      }
      if (tmpBlock_->name2term_.find(term->getName())
          == tmpBlock_->name2term_.end()) {
        logger_->error(DRT, 104, "Terminal {} not found.", term->getName());
      }
      auto frbterm = tmpBlock_->name2term_[term->getName()];  // frBTerm*
      frbterm->addToNet(netIn);
      netIn->addBTerm(frbterm);
      if (!is_special) {
        // graph enablement
        auto termNode = std::make_unique<frNode>();
        termNode->setPin(frbterm);
        termNode->setType(frNodeTypeEnum::frcPin);
        netIn->addNode(termNode);
      }
    }
    for (auto term : net->getITerms()) {
      if (term->getSigType().isSupply() && !net->getSigType().isSupply()) {
        logger_->error(DRT,
                       307,
                       "Net {} of signal type {} cannot be connected to iterm "
                       "{} with signal type {}",
                       net->getName(),
                       net->getSigType().getString(),
                       term->getName(),
                       term->getSigType().getString());
      }
      if (tmpBlock_->name2inst_.find(term->getInst()->getName())
          == tmpBlock_->name2inst_.end()) {
        logger_->error(
            DRT, 105, "Component {} not found.", term->getInst()->getName());
      }
      auto inst = tmpBlock_->name2inst_[term->getInst()->getName()];
      // gettin inst term
      auto frterm = inst->getMaster()->getTerm(term->getMTerm()->getName());
      if (frterm == nullptr) {
        logger_->error(
            DRT, 106, "Component pin {} not found.", term->getName());
      }
      int idx = frterm->getIndexInOwner();
      auto& instTerms = inst->getInstTerms();
      auto instTerm = instTerms[idx].get();
      assert(instTerm->getTerm()->getName() == term->getMTerm()->getName());

      instTerm->addToNet(netIn);
      netIn->addInstTerm(instTerm);
      if (!is_special) {
        // graph enablement
        auto instTermNode = std::make_unique<frNode>();
        instTermNode->setPin(instTerm);
        instTermNode->setType(frNodeTypeEnum::frcPin);
        netIn->addNode(instTermNode);
      }
    }
    // initialize
    std::string layerName;
    std::string viaName;
    std::string shape;
    bool hasBeginPoint = false;
    bool hasEndPoint = false;
    bool orthogonal_conn = false;
    bool beginInVia = false;
    frCoord beginX = -1;
    frCoord beginY = -1;
    frCoord beginExt = -1;
    frCoord nextX = -1;
    frCoord nextY = -1;
    frCoord endX = -1;
    frCoord endY = -1;
    frCoord endExt = -1;
    odb::dbTechLayer* prevLayer = nullptr;
    odb::dbTechLayer* lower_layer = nullptr;
    odb::dbTechLayer* top_layer = nullptr;
    bool hasRect = false;
    frCoord left = -1;
    frCoord bottom = -1;
    frCoord right = -1;
    frCoord top = -1;
    frCoord width = 0;
    odb::dbWireDecoder decoder;

    if (!net->isSpecial() && net->getWire() != nullptr) {
      decoder.begin(net->getWire());
      odb::dbWireDecoder::OpCode pathId = decoder.next();
      while (pathId != odb::dbWireDecoder::END_DECODE) {
        // for each path start
        // when previous connection has a different direction of the current
        // connection, use the last end point as the new begin point. it avoids
        // missing segments between the connections with different directions.
        if (orthogonal_conn) {
          hasBeginPoint = true;
          beginX = endX;
          beginY = endY;
        } else {
          layerName = "";
          hasBeginPoint = false;
          beginX = -1;
          beginY = -1;
        }
        viaName = "";
        shape = "";
        hasEndPoint = false;
        beginInVia = false;
        orthogonal_conn = false;
        beginExt = -1;
        endX = -1;
        endY = -1;
        endExt = -1;
        hasRect = false;
        left = -1;
        bottom = -1;
        right = -1;
        top = -1;
        width = 0;
        bool endpath = false;
        do {
          switch (pathId) {
            case odb::dbWireDecoder::PATH:
            case odb::dbWireDecoder::JUNCTION:
            case odb::dbWireDecoder::SHORT:
            case odb::dbWireDecoder::VWIRE:
              prevLayer = decoder.getLayer();
              layerName = prevLayer->getName();
              if (tech_->name2layer_.find(layerName)
                  == tech_->name2layer_.end()) {
                logger_->error(DRT, 107, "Unsupported layer {}.", layerName);
              }
              break;
            case odb::dbWireDecoder::POINT:
              if (!hasBeginPoint) {
                decoder.getPoint(beginX, beginY);
                hasBeginPoint = true;
              } else {
                decoder.getPoint(endX, endY);
                hasEndPoint = true;
              }
              break;
            case odb::dbWireDecoder::POINT_EXT:
              if (!hasBeginPoint) {
                decoder.getPoint(beginX, beginY, beginExt);
                hasBeginPoint = true;
              } else {
                decoder.getPoint(endX, endY, endExt);
                hasEndPoint = true;
              }
              break;
            case odb::dbWireDecoder::VIA:
              viaName = std::string(decoder.getVia()->getName());
              lower_layer = decoder.getVia()->getBottomLayer();
              top_layer = decoder.getVia()->getTopLayer();
              layerName = prevLayer == top_layer ? lower_layer->getName()
                                                 : top_layer->getName();
              if (!hasBeginPoint) {
                beginX = nextX;
                beginY = nextY;
                hasBeginPoint = true;
                beginInVia = true;
              }
              break;
            case odb::dbWireDecoder::TECH_VIA:
              viaName = std::string(decoder.getTechVia()->getName());
              lower_layer = decoder.getTechVia()->getBottomLayer();
              top_layer = decoder.getTechVia()->getTopLayer();
              layerName = prevLayer == top_layer ? lower_layer->getName()
                                                 : top_layer->getName();
              if (!hasBeginPoint) {
                beginX = nextX;
                beginY = nextY;
                hasBeginPoint = true;
                beginInVia = true;
              }
              break;
            case odb::dbWireDecoder::RECT:
              decoder.getRect(left, bottom, right, top);
              hasRect = true;
              break;
            case odb::dbWireDecoder::ITERM:
            case odb::dbWireDecoder::BTERM:
            case odb::dbWireDecoder::RULE:
            case odb::dbWireDecoder::END_DECODE:
              break;
            default:
              break;
          }
          pathId = decoder.next();

          if (pathId == odb::dbWireDecoder::POINT && hasEndPoint) {
            frCoord x, y;
            decoder.getPoint(x, y);
            bool curr_conn_vertical = beginX == endX;
            bool next_conn_vertical = endX == x;
            orthogonal_conn = curr_conn_vertical != next_conn_vertical;
          }

          if ((int) pathId <= 3 || pathId == odb::dbWireDecoder::TECH_VIA
              || pathId == odb::dbWireDecoder::VIA
              || pathId == odb::dbWireDecoder::END_DECODE || orthogonal_conn) {
            if (hasEndPoint) {
              nextX = endX;
              nextY = endY;
            } else {
              nextX = beginX;
              nextY = beginY;
            }
            endpath = true;
          }
        } while (!endpath);
        auto layerNum = tech_->name2layer_[layerName]->getLayerNum();
        if (hasRect) {
          continue;
        }
        if (hasEndPoint) {
          auto tmpP = std::make_unique<frPathSeg>();
          if (beginX > endX || beginY > endY) {
            tmpP->setPoints(Point(endX, endY), Point(beginX, beginY));
            std::swap(beginExt, endExt);
          } else {
            tmpP->setPoints(Point(beginX, beginY), Point(endX, endY));
          }
          tmpP->addToNet(netIn);
          tmpP->setLayerNum(layerNum);
          auto layer = tech_->name2layer_[layerName];
          auto styleWidth = width;
          if (!(styleWidth)) {
            if ((layer->isHorizontal() && beginY != endY)
                || (!layer->isHorizontal() && beginX != endX)) {
              styleWidth = layer->getWrongDirWidth();
            } else {
              styleWidth = layer->getWidth();
            }
          }
          width = (width) ? width : tech_->name2layer_[layerName]->getWidth();
          auto defaultBeginExt = width / 2;
          auto defaultEndExt = width / 2;

          frEndStyleEnum tmpBeginEnum;
          if (beginExt == -1) {
            tmpBeginEnum = frcExtendEndStyle;
          } else if (beginExt == 0) {
            tmpBeginEnum = frcTruncateEndStyle;
          } else {
            tmpBeginEnum = frcVariableEndStyle;
          }
          frEndStyle tmpBeginStyle(tmpBeginEnum);

          frEndStyleEnum tmpEndEnum;
          if (endExt == -1) {
            tmpEndEnum = frcExtendEndStyle;
          } else if (endExt == 0) {
            tmpEndEnum = frcTruncateEndStyle;
          } else {
            tmpEndEnum = frcVariableEndStyle;
          }
          frEndStyle tmpEndStyle(tmpEndEnum);

          frSegStyle tmpSegStyle;
          tmpSegStyle.setWidth(styleWidth);
          tmpSegStyle.setBeginStyle(
              tmpBeginStyle,
              tmpBeginEnum == frcExtendEndStyle ? defaultBeginExt : beginExt);
          tmpSegStyle.setEndStyle(
              tmpEndStyle,
              tmpEndEnum == frcExtendEndStyle ? defaultEndExt : endExt);
          tmpP->setStyle(tmpSegStyle);
          netIn->addShape(std::move(tmpP));
        }
        if (!viaName.empty()) {
          if (tech_->name2via_.find(viaName) == tech_->name2via_.end()) {
            logger_->error(DRT, 108, "Unsupported via in db.");
          } else {
            Point p;
            if (hasEndPoint && !beginInVia) {
              p = {endX, endY};
            } else {
              p = {beginX, beginY};
            }
            auto viaDef = tech_->name2via_[viaName];
            auto tmpP = std::make_unique<frVia>(viaDef);
            tmpP->setOrigin(p);
            tmpP->addToNet(netIn);
            netIn->addVia(std::move(tmpP));
          }
        }
        // for each path end
      }
    }
    if (net->isSpecial()) {
      for (auto swire : net->getSWires()) {
        for (auto box : swire->getWires()) {
          if (!box->isVia()) {
            getSBoxCoords(box, beginX, beginY, endX, endY, width);
            auto layerNum = tech_->name2layer_[box->getTechLayer()->getName()]
                                ->getLayerNum();
            auto tmpP = std::make_unique<frPathSeg>();
            tmpP->setPoints(Point(beginX, beginY), Point(endX, endY));
            tmpP->addToNet(netIn);
            tmpP->setLayerNum(layerNum);
            width = (width) ? width : tech_->name2layer_[layerName]->getWidth();
            auto defaultExt = width / 2;

            frEndStyleEnum tmpBeginEnum;
            if (box->getWireShapeType() == odb::dbWireShapeType::NONE) {
              tmpBeginEnum = frcExtendEndStyle;
            } else {
              tmpBeginEnum = frcTruncateEndStyle;
            }
            frEndStyle tmpBeginStyle(tmpBeginEnum);
            frEndStyleEnum tmpEndEnum;
            if (box->getWireShapeType() == odb::dbWireShapeType::NONE) {
              tmpEndEnum = frcExtendEndStyle;
            } else {
              tmpEndEnum = frcTruncateEndStyle;
            }
            frEndStyle tmpEndStyle(tmpEndEnum);

            frSegStyle tmpSegStyle;
            tmpSegStyle.setWidth(width);
            tmpSegStyle.setBeginStyle(
                tmpBeginStyle,
                tmpBeginEnum == frcExtendEndStyle ? defaultExt : 0);
            tmpSegStyle.setEndStyle(
                tmpEndStyle, tmpEndEnum == frcExtendEndStyle ? defaultExt : 0);
            tmpP->setStyle(tmpSegStyle);
            netIn->addShape(std::move(tmpP));
          } else {
            if (box->getTechVia()) {
              viaName = box->getTechVia()->getName();
            } else if (box->getBlockVia()) {
              viaName = box->getBlockVia()->getName();
            }

            if (tech_->name2via_.find(viaName) == tech_->name2via_.end()) {
              logger_->error(DRT, 109, "Unsupported via in db.");
            } else {
              int x, y;
              box->getViaXY(x, y);
              Point p(x, y);
              auto viaDef = tech_->name2via_[viaName];
              auto tmpP = std::make_unique<frVia>(viaDef);
              tmpP->setOrigin(p);
              tmpP->addToNet(netIn);
              netIn->addVia(std::move(tmpP));
            }
          }
        }
      }
    }
    netIn->setType(net->getSigType());
    if (is_special) {
      tmpBlock_->addSNet(std::move(uNetIn));
    } else {
      tmpBlock_->addNet(std::move(uNetIn));
    }
  }
}

void updatefrAccessPoint(odb::dbAccessPoint* db_ap,
                         frAccessPoint* ap,
                         frTechObject* tech)
{
  ap->setPoint(db_ap->getPoint());
  if (db_ap->hasAccess(odb::dbDirection::NORTH)) {
    ap->setAccess(frDirEnum::N, true);
  }
  if (db_ap->hasAccess(odb::dbDirection::SOUTH)) {
    ap->setAccess(frDirEnum::S, true);
  }
  if (db_ap->hasAccess(odb::dbDirection::EAST)) {
    ap->setAccess(frDirEnum::E, true);
  }
  if (db_ap->hasAccess(odb::dbDirection::WEST)) {
    ap->setAccess(frDirEnum::W, true);
  }
  if (db_ap->hasAccess(odb::dbDirection::UP)) {
    ap->setAccess(frDirEnum::U, true);
  }
  if (db_ap->hasAccess(odb::dbDirection::DOWN)) {
    ap->setAccess(frDirEnum::D, true);
  }
  auto layer = tech->getLayer(db_ap->getLayer()->getName());
  ap->setLayer(layer->getLayerNum());
  ap->setType((frAccessPointEnum) db_ap->getLowType().getValue(), true);
  ap->setType((frAccessPointEnum) db_ap->getHighType().getValue(), false);
  auto viadefs = db_ap->getVias();
  for (const auto& cutViaDefs : viadefs) {
    for (const auto& via : cutViaDefs) {
      if (via->getObjectType() == odb::dbObjectType::dbViaObj) {
        auto blockVia = static_cast<odb::dbVia*>(via);
        auto viadef = tech->getVia(blockVia->getName());
        ap->addViaDef(viadef);
      } else {
        auto techVia = static_cast<odb::dbTechVia*>(via);
        auto viadef = tech->getVia(techVia->getName());
        ap->addViaDef(viadef);
      }
    }
  }
  auto db_path_segs = db_ap->getSegments();
  for (const auto& [db_rect, begin_style_trunc, end_style_trunc] :
       db_path_segs) {
    frPathSeg path_seg;
    path_seg.setPoints_safe(db_rect.ll(), db_rect.ur());
    if (begin_style_trunc == true) {
      path_seg.setBeginStyle(frcTruncateEndStyle);
    }
    if (end_style_trunc == true) {
      path_seg.setEndStyle(frcTruncateEndStyle);
    }

    ap->addPathSeg(path_seg);
  }
}

void io::Parser::setBTerms(odb::dbBlock* block)
{
  for (auto term : block->getBTerms()) {
    switch (term->getSigType().getValue()) {
      case odb::dbSigType::POWER:
      case odb::dbSigType::GROUND:
        // We allow for multiple pins
        break;
      case odb::dbSigType::TIEOFF:
      case odb::dbSigType::SIGNAL:
      case odb::dbSigType::CLOCK:
      case odb::dbSigType::ANALOG:
      case odb::dbSigType::RESET:
      case odb::dbSigType::SCAN:
        if (term->getBPins().size() > 1) {
          logger_->error(utl::DRT,
                         302,
                         "Unsupported multiple pins on bterm {}",
                         term->getName());
        }
        break;
    }
    auto uTermIn = std::make_unique<frBTerm>(term->getName());
    auto termIn = uTermIn.get();
    termIn->setId(numTerms_);
    numTerms_++;
    termIn->setType(term->getSigType());
    termIn->setDirection(term->getIoType());
    auto pinIn = std::make_unique<frBPin>();
    pinIn->setId(0);

    int bterm_bottom_layer_idx = std::numeric_limits<int>::max();
    for (auto bpin : term->getBPins()) {
      for (auto box : bpin->getBoxes()) {
        frLayerNum layer_idx
            = tech_->name2layer_[box->getTechLayer()->getName()]->getLayerNum();
        bterm_bottom_layer_idx = std::min(bterm_bottom_layer_idx, layer_idx);
      }
    }

    if (bterm_bottom_layer_idx > TOP_ROUTING_LAYER
        && term->getNet()->getWire() != nullptr) {
      frLayerNum finalLayerNum = 0;
      odb::Rect bbox = getViaBoxForTermAboveMaxLayer(term, finalLayerNum);
      termIn->setIsAboveTopLayer(true);
      setBTerms_addPinFig_helper(pinIn.get(), bbox, finalLayerNum);
    } else {
      for (auto pin : term->getBPins()) {
        for (auto box : pin->getBoxes()) {
          odb::Rect bbox = box->getBox();
          if (tech_->name2layer_.find(box->getTechLayer()->getName())
              == tech_->name2layer_.end()) {
            logger_->error(DRT,
                           112,
                           "Unsupported layer {}.",
                           box->getTechLayer()->getName());
          }
          frLayerNum layerNum
              = tech_->name2layer_[box->getTechLayer()->getName()]
                    ->getLayerNum();
          frLayerNum finalLayerNum = layerNum;
          setBTerms_addPinFig_helper(pinIn.get(), bbox, finalLayerNum);
        }
      }
    }

    auto pa = std::make_unique<frPinAccess>();
    if (!term->getSigType().isSupply() && term->getBPins().size() == 1) {
      auto db_pin = (odb::dbBPin*) *term->getBPins().begin();
      for (auto& db_ap : db_pin->getAccessPoints()) {
        auto ap = std::make_unique<frAccessPoint>();
        updatefrAccessPoint(db_ap, ap.get(), tech_);
        pa->addAccessPoint(std::move(ap));
      }
    }
    pinIn->addPinAccess(std::move(pa));
    termIn->addPin(std::move(pinIn));
    tmpBlock_->addTerm(std::move(uTermIn));
  }
}

odb::Rect io::Parser::getViaBoxForTermAboveMaxLayer(odb::dbBTerm* term,
                                                    frLayerNum& finalLayerNum)
{
  odb::dbNet* net = term->getNet();
  odb::dbWire* wire = net->getWire();
  odb::Rect bbox = term->getBBox();
  if (wire != nullptr) {
    odb::dbWirePath path;
    odb::dbWirePathShape pshape;
    odb::dbWirePathItr pitr;
    for (pitr.begin(wire); pitr.getNextPath(path);) {
      while (pitr.getNextShape(pshape)) {
        if (pshape.shape.isVia()) {
          odb::dbTechVia* via = pshape.shape.getTechVia();
          for (const auto& vbox : via->getBoxes()) {
            frLayerNum layerNum
                = tech_->name2layer_[vbox->getTechLayer()->getName()]
                      ->getLayerNum();
            if (layerNum == TOP_ROUTING_LAYER) {
              odb::Rect viaBox = vbox->getBox();
              odb::dbTransform xform;
              odb::Point path_origin = pshape.point;
              xform.setOffset({path_origin.x(), path_origin.y()});
              xform.setOrient(odb::dbOrientType(odb::dbOrientType::R0));
              xform.apply(viaBox);
              if (bbox.intersects(viaBox)) {
                bbox = viaBox;
                finalLayerNum = layerNum;
                break;
              }
            }
          }
        }
      }
    }
  }

  return bbox;
}

void io::Parser::setBTerms_addPinFig_helper(frBPin* pinIn,
                                            odb::Rect bbox,
                                            frLayerNum finalLayerNum)
{
  std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
  pinFig->setBBox(bbox);
  pinFig->addToPin(pinIn);
  pinFig->setLayerNum(finalLayerNum);
  std::unique_ptr<frPinFig> uptr(std::move(pinFig));
  pinIn->addPinFig(std::move(uptr));
}

void io::Parser::setAccessPoints(odb::dbDatabase* db)
{
  std::map<odb::dbAccessPoint*, frAccessPoint*> ap_map;
  for (auto& master : design_->getMasters()) {
    auto db_master = db->findMaster(master->getName().c_str());
    for (auto& term : master->getTerms()) {
      auto db_mterm = db_master->findMTerm(term->getName().c_str());
      if (db_mterm == nullptr) {
        logger_->error(DRT, 404, "mterm {} not found in db", term->getName());
      }
      auto db_pins = db_mterm->getMPins();
      if (db_pins.size() != term->getPins().size()) {
        logger_->error(DRT,
                       405,
                       "Mismatch in number of pins for term {}/{}",
                       master->getName(),
                       term->getName());
      }
      frUInt4 i = 0;
      auto& pins = term->getPins();
      for (auto db_pin : db_pins) {
        auto& pin = pins[i++];
        auto db_pas = db_pin->getPinAccess();
        for (const auto& db_aps : db_pas) {
          std::unique_ptr<frPinAccess> pa = std::make_unique<frPinAccess>();
          for (auto db_ap : db_aps) {
            std::unique_ptr<frAccessPoint> ap
                = std::make_unique<frAccessPoint>();
            updatefrAccessPoint(db_ap, ap.get(), tech_);
            ap_map[db_ap] = ap.get();
            pa->addAccessPoint(std::move(ap));
          }
          pin->addPinAccess(std::move(pa));
        }
      }
    }
  }
  for (auto db_inst : db->getChip()->getBlock()->getInsts()) {
    auto inst = tmpBlock_->findInst(db_inst->getName());
    if (inst == nullptr) {
      continue;
    }
    for (auto db_term : db_inst->getITerms()) {
      auto term = inst->getInstTerm(db_term->getMTerm()->getName());
      if (term == nullptr) {
        continue;
      }

      auto db_aps = db_term->getPrefAccessPoints();
      std::map<odb::dbMPin*, odb::dbAccessPoint*> db_aps_map;
      for (auto db_ap : db_aps) {
        if (ap_map.find(db_ap) == ap_map.end()) {
          logger_->error(DRT,
                         1011,
                         "Access Point not found for iterm {}",
                         db_term->getName());
        }
        db_aps_map[db_ap->getMPin()] = db_ap;
      }
      int idx = 0;
      for (auto mpin : db_term->getMTerm()->getMPins()) {
        if (db_aps_map.find(mpin) == db_aps_map.end()) {
          term->setAccessPoint(idx, nullptr);
        } else {
          term->setAccessPoint(idx, ap_map[db_aps_map[mpin]]);
        }
        ++idx;
      }
    }
  }
}

void io::Parser::readDesign(odb::dbDatabase* db)
{
  if (design_->getTopBlock() != nullptr) {
    return;
  }
  if (VERBOSE > 0) {
    logger_->info(DRT, 150, "Reading design.");
  }

  ProfileTask profile("IO:readDesign");
  if (db->getChip() == nullptr) {
    logger_->error(DRT, 116, "Load design first.");
  }
  odb::dbBlock* block = db->getChip()->getBlock();
  if (block == nullptr) {
    logger_->error(DRT, 117, "Load design first.");
  }
  tmpBlock_ = std::make_unique<frBlock>(std::string(block->getName()));
  tmpBlock_->trackPatterns_.clear();
  tmpBlock_->trackPatterns_.resize(tech_->layers_.size());
  setDieArea(block);
  setTracks(block);
  setInsts(block);
  setObstructions(block);
  setBTerms(block);
  setAccessPoints(db);
  setNets(block);
  tmpBlock_->setId(0);
  design_->setTopBlock(std::move(tmpBlock_));
  addFakeNets();

  auto numLefVia = tech_->vias_.size();
  if (VERBOSE > 0) {
    logger_->report("");
    Rect dieBox = design_->getTopBlock()->getDieBox();
    logger_->report("Design:                   {}",
                    design_->getTopBlock()->getName());
    // TODO Rect can't be logged directly
    std::stringstream dieBoxSStream;
    dieBoxSStream << dieBox;
    logger_->report("Die area:                 {}", dieBoxSStream.str());
    logger_->report("Number of track patterns: {}",
                    design_->getTopBlock()->getTrackPatterns().size());
    logger_->report("Number of DEF vias:       {}",
                    tech_->vias_.size() - numLefVia);
    logger_->report("Number of components:     {}",
                    design_->getTopBlock()->insts_.size());
    logger_->report("Number of terminals:      {}",
                    design_->getTopBlock()->terms_.size());
    logger_->report("Number of snets:          {}",
                    design_->getTopBlock()->snets_.size());
    logger_->report("Number of nets:           {}",
                    design_->getTopBlock()->nets_.size());
    logger_->report("");
    logger_->metric("route__net", design_->getTopBlock()->nets_.size());
    logger_->metric("route__net__special",
                    design_->getTopBlock()->snets_.size());
  }
}

void io::Parser::addFakeNets()
{
  // add VSS fake net
  auto vssFakeNet = std::make_unique<frNet>(std::string("frFakeVSS"));
  vssFakeNet->setType(dbSigType::GROUND);
  vssFakeNet->setIsFake(true);
  design_->getTopBlock()->addFakeSNet(std::move(vssFakeNet));
  // add VDD fake net
  auto vddFakeNet = std::make_unique<frNet>(std::string("frFakeVDD"));
  vddFakeNet->setType(dbSigType::POWER);
  vddFakeNet->setIsFake(true);
  design_->getTopBlock()->addFakeSNet(std::move(vddFakeNet));
}

void io::Parser::setRoutingLayerProperties(odb::dbTechLayer* layer,
                                           frLayer* tmpLayer)
{
  for (auto rule : layer->getTechLayerCornerSpacingRules()) {
    std::string widthName("WIDTH");
    std::vector<frCoord> widths;
    std::vector<std::pair<frCoord, frCoord>> spacings;
    rule->getSpacingTable(spacings);
    rule->getWidthTable(widths);
    bool hasSameXY = true;
    for (auto& [spacing1, spacing2] : spacings) {
      if (spacing1 != spacing2) {
        hasSameXY = false;
      }
    }

    fr1DLookupTbl<frCoord, std::pair<frCoord, frCoord>> cornerSpacingTbl(
        widthName, widths, spacings);
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frLef58CornerSpacingConstraint>(cornerSpacingTbl);
    auto rptr = static_cast<frLef58CornerSpacingConstraint*>(uCon.get());
    switch (rule->getType()) {
      case odb::dbTechLayerCornerSpacingRule::CornerType::CONVEXCORNER:
        rptr->setCornerType(frCornerTypeEnum::CONVEX);
        rptr->setSameMask(rule->isSameMask());
        if (rule->isCornerOnly()) {
          rptr->setWithin(rule->getWithin());
        } else if (rule->isCornerToCorner()) {
          rptr->setCornerToCorner(true);
        }
        if (rule->isExceptEol()) {
          rptr->setEolWidth(rule->getEolWidth());
          if (rule->isExceptJogLength()) {
            rptr->setLength(rule->getJogLength());
            rptr->setEdgeLength(rule->isEdgeLengthValid());
            rptr->setIncludeLShape(rule->isIncludeShape());
          }
        }

        break;

      default:
        rptr->setCornerType(frCornerTypeEnum::CONCAVE);
        if (rule->isMinLengthValid()) {
          rptr->setMinLength(rule->getMinLength());
        }
        rptr->setExceptNotch(rule->isExceptNotch());
        if (rule->isExceptNotchLengthValid()) {
          rptr->setExceptNotchLength(rule->getExceptNotchLength());
        }
        break;
    }
    rptr->setSameXY(hasSameXY);
    rptr->setExceptSameNet(rule->isExceptSameNet());
    rptr->setExceptSameMetal(rule->isExceptSameMetal());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addLef58CornerSpacingConstraint(rptr);
  }
  for (auto rule : layer->getTechLayerSpacingTablePrlRules()) {
    std::string rowName("WIDTH");
    std::string colName("PARALLELRUNLENGTH");
    frCollection<frCoord> rowVals, colVals;
    frCollection<frCollection<frCoord>> tblVals;
    std::map<frCoord, std::pair<frCoord, frCoord>> ewVals;
    std::map<frUInt4, std::pair<frCoord, frCoord>> _ewVals;
    rule->getTable(rowVals, colVals, tblVals, _ewVals);
    for (auto& [key, value] : _ewVals) {
      ewVals[key] = value;
    }
    std::shared_ptr<fr2DLookupTbl<frCoord, frCoord, frCoord>> prlTbl
        = std::make_shared<fr2DLookupTbl<frCoord, frCoord, frCoord>>(
            rowName, rowVals, colName, colVals, tblVals);
    std::unique_ptr<frLef58SpacingTableConstraint> spacingTableConstraint
        = std::make_unique<frLef58SpacingTableConstraint>(prlTbl, ewVals);
    spacingTableConstraint->setWrongDirection(rule->isWrongDirection());
    spacingTableConstraint->setSameMask(rule->isSameMask());
    if (rule->isExceeptEol()) {
      spacingTableConstraint->setEolWidth(rule->getEolWidth());
    }
    tmpLayer->addConstraint(spacingTableConstraint.get());
    tech_->addUConstraint(std::move(spacingTableConstraint));
  }
  for (auto rule : layer->getTechLayerSpacingEolRules()) {
    if (rule->isExceptExactWidthValid()) {
      logger_->warn(utl::DRT,
                    400,
                    "Unsupported LEF58_SPACING rule with option "
                    "EXCEPTEXACTWIDTH for layer {}.",
                    layer->getName());
      continue;
    }
    if (rule->isFillConcaveCornerValid()) {
      logger_->warn(utl::DRT,
                    401,
                    "Unsupported LEF58_SPACING rule with option "
                    "FILLCONCAVECORNER for layer {}.",
                    layer->getName());
      continue;
    }
    if (rule->isEqualRectWidthValid()) {
      logger_->warn(utl::DRT,
                    403,
                    "Unsupported LEF58_SPACING rule with option EQUALRECTWIDTH "
                    "for layer {}.",
                    layer->getName());
      continue;
    }
    auto con = std::make_unique<frLef58SpacingEndOfLineConstraint>();
    con->setEol(
        rule->getEolSpace(), rule->getEolWidth(), rule->isExactWidthValid());
    if (rule->isWrongDirSpacingValid()) {
      con->setWrongDirSpace(rule->getWrongDirSpace());
    }

    auto within = std::make_shared<frLef58SpacingEndOfLineWithinConstraint>();
    con->setWithinConstraint(within);
    if (rule->isOppositeWidthValid()) {
      within->setOppositeWidth(rule->getOppositeWidth());
    }
    if (rule->isEndPrlSpacingValid()) {
      within->setEndPrl(rule->getEndPrlSpace(), rule->getEndPrl());
    }
    within->setEolWithin(rule->getEolWithin());
    if (rule->isWrongDirWithinValid()) {
      within->setWrongDirWithin(rule->getWrongDirWithin());
    }
    if (rule->isSameMaskValid()) {
      within->setSameMask(rule->isSameMaskValid());
    }
    if (rule->isEndToEndValid()) {
      auto endToEnd
          = std::make_shared<frLef58SpacingEndOfLineWithinEndToEndConstraint>();
      within->setEndToEndConstraint(endToEnd);
      endToEnd->setEndToEndSpace(rule->getEndToEndSpace());
      endToEnd->setCutSpace(rule->getOneCutSpace(), rule->getTwoCutSpace());
      if (rule->isExtensionValid()) {
        if (rule->isWrongDirExtensionValid()) {
          endToEnd->setExtension(rule->getExtension(),
                                 rule->getWrongDirExtension());
        } else {
          endToEnd->setExtension(rule->getExtension());
        }
      }
      if (rule->isOtherEndWidthValid()) {
        endToEnd->setOtherEndWidth(rule->getOtherEndWidth());
      }
    }
    if (rule->isParallelEdgeValid()) {
      auto parallelEdge = std::make_shared<
          frLef58SpacingEndOfLineWithinParallelEdgeConstraint>();
      within->setParallelEdgeConstraint(parallelEdge);
      if (rule->isSubtractEolWidthValid()) {
        parallelEdge->setSubtractEolWidth(rule->isSubtractEolWidthValid());
      }
      parallelEdge->setPar(rule->getParSpace(), rule->getParWithin());
      if (rule->isParPrlValid()) {
        parallelEdge->setPrl(rule->getParPrl());
      }
      if (rule->isParMinLengthValid()) {
        parallelEdge->setMinLength(rule->getParMinLength());
      }
      if (rule->isTwoEdgesValid()) {
        parallelEdge->setTwoEdges(rule->isTwoEdgesValid());
      }
      if (rule->isSameMetalValid()) {
        parallelEdge->setSameMetal(rule->isSameMetalValid());
      }
      if (rule->isNonEolCornerOnlyValid()) {
        parallelEdge->setNonEolCornerOnly(rule->isNonEolCornerOnlyValid());
      }
      if (rule->isParallelSameMaskValid()) {
        parallelEdge->setParallelSameMask(rule->isParallelSameMaskValid());
      }
    }
    if (rule->isMinLengthValid() || rule->isMaxLengthValid()) {
      auto len = std::make_shared<
          frLef58SpacingEndOfLineWithinMaxMinLengthConstraint>();
      within->setMaxMinLengthConstraint(len);
      if (rule->isMinLengthValid()) {
        len->setLength(false, rule->getMinLength(), rule->isTwoEdgesValid());
      } else {
        len->setLength(true, rule->getMaxLength(), rule->isTwoEdgesValid());
      }
    }
    if (rule->isEncloseCutValid()) {
      auto enc
          = std::make_shared<frLef58SpacingEndOfLineWithinEncloseCutConstraint>(
              rule->getEncloseDist(), rule->getCutToMetalSpace());
      within->setEncloseCutConstraint(enc);
      enc->setAbove(rule->isAboveValid());
      enc->setBelow(rule->isBelowValid());
      enc->setAllCuts(rule->isAllCutsValid());
    }
    tmpLayer->addLef58SpacingEndOfLineConstraint(con.get());
    tech_->addUConstraint(std::move(con));
  }
  for (auto rule : layer->getTechLayerWrongDirSpacingRules()) {
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frLef58SpacingWrongDirConstraint>(rule);
    auto rptr = static_cast<frLef58SpacingWrongDirConstraint*>(uCon.get());
    if (rule->isLengthValid()) {
      logger_->warn(
          DRT, 333, "Unsupported branch LENGTH in WrongDirSpacingRule.");
      continue;
    }
    if (rule->getPrlLength() < 0) {
      logger_->warn(DRT,
                    334,
                    "Unsupported negative value for prlLength in branch PRL "
                    "in WrongDirSpacingRule.");
      continue;
    }
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addLef58SpacingWrongDirConstraint(rptr);
  }
  if (tech_->hasUnidirectionalLayer(layer)) {
    tmpLayer->setUnidirectional(true);
  }
  if (layer->isRectOnly()) {
    auto rectOnlyConstraint = std::make_unique<frLef58RectOnlyConstraint>(
        layer->isRectOnlyExceptNonCorePins());
    tmpLayer->setLef58RectOnlyConstraint(rectOnlyConstraint.get());
    tech_->addUConstraint(std::move(rectOnlyConstraint));
  }
  // We don't handle coloring so tracks on any multiple patterned
  // layer are forced to be on grid.
  if (layer->isRightWayOnGridOnly() || layer->getNumMasks() > 1) {
    auto rightWayOnGridOnlyConstraint
        = std::make_unique<frLef58RightWayOnGridOnlyConstraint>(
            layer->isRightWayOnGridOnlyCheckMask());
    tmpLayer->setLef58RightWayOnGridOnlyConstraint(
        rightWayOnGridOnlyConstraint.get());
    tech_->addUConstraint(std::move(rightWayOnGridOnlyConstraint));
  }
  for (auto rule : layer->getTechLayerMinStepRules()) {
    if (rule->getMaxEdges() > 1) {
      logger_->warn(DRT,
                    335,
                    "LEF58_MINSTEP MAXEDGES {}  is not supported",
                    rule->getMaxEdges());
      continue;
    }
    auto con = std::make_unique<frLef58MinStepConstraint>();
    con->setMinStepLength(rule->getMinStepLength());
    con->setMaxEdges(rule->isMaxEdgesValid() ? rule->getMaxEdges() : -1);
    con->setMinAdjacentLength(
        rule->isMinAdjLength1Valid() ? rule->getMinAdjLength1() : -1);
    con->setNoAdjEol(rule->isNoAdjacentEol() ? rule->getEolWidth() : -1);
    con->setEolWidth(rule->isNoBetweenEol() ? rule->getEolWidth() : -1);
    con->setExceptRectangle(rule->isExceptRectangle());
    tmpLayer->addLef58MinStepConstraint(con.get());
    tech_->addUConstraint(std::move(con));
  }
  for (auto rule : layer->getTechLayerEolExtensionRules()) {
    frCollection<frCoord> widthTbl;
    frCollection<frCoord> extTbl;
    frCollection<std::pair<frCoord, frCoord>> dbExtTbl;
    rule->getExtensionTable(dbExtTbl);
    for (auto& [width, ext] : dbExtTbl) {
      widthTbl.push_back(width);
      extTbl.push_back(ext);
    }
    auto con = std::make_unique<frLef58EolExtensionConstraint>(
        fr1DLookupTbl<frCoord, frCoord>("WIDTH", widthTbl, extTbl, false));
    con->setMinSpacing(rule->getSpacing());
    con->setParallelOnly(rule->isParallelOnly());
    tmpLayer->addLef58EolExtConstraint(con.get());
    tech_->addUConstraint(std::move(con));
  }
  for (auto rule : layer->getTechLayerAreaRules()) {
    auto con = std::make_unique<frLef58AreaConstraint>(rule);
    tmpLayer->addLef58AreaConstraint(con.get());
    tech_->addUConstraint(std::move(con));
  }
  for (auto rule : layer->getTechLayerTwoWiresForbiddenSpcRules()) {
    auto con = std::make_unique<frLef58TwoWiresForbiddenSpcConstraint>(rule);
    tmpLayer->addTwoWiresForbiddenSpacingConstraint(con.get());
    tech_->addUConstraint(std::move(con));
  }
}

void io::Parser::setCutLayerProperties(odb::dbTechLayer* layer,
                                       frLayer* tmpLayer)
{
  for (auto rule : layer->getTechLayerCutClassRules()) {
    auto cutClass = std::make_unique<frLef58CutClass>();
    std::string name = rule->getName();
    cutClass->setName(name);
    cutClass->setViaWidth(rule->getWidth());
    if (rule->isLengthValid()) {
      cutClass->setViaLength(rule->getLength());
    } else {
      cutClass->setViaLength(rule->getWidth());
    }
    if (rule->isCutsValid()) {
      cutClass->setNumCut(rule->getNumCuts());
    } else {
      cutClass->setNumCut(1);
    }
    tech_->addCutClass(tmpLayer->getLayerNum(), std::move((cutClass)));
  }
  for (auto rule : layer->getTechLayerCutSpacingRules()) {
    switch (rule->getType()) {
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::ADJACENTCUTS: {
        auto con = std::make_unique<frLef58CutSpacingConstraint>();
        con->setCutSpacing(rule->getCutSpacing());
        con->setCenterToCenter(rule->isCenterToCenter());
        con->setSameNet(rule->isSameNet());
        con->setSameMetal(rule->isSameMetal());
        con->setSameVia(rule->isSameVia());
        con->setAdjacentCuts(rule->getAdjacentCuts());
        if (rule->isExactAligned()) {
          con->setExactAlignedCut(rule->getNumCuts());
        }
        if (rule->isTwoCutsValid()) {
          con->setTwoCuts(rule->getTwoCuts());
        }
        con->setSameCut(rule->isSameCut());
        con->setCutWithin(rule->getWithin());
        con->setExceptSamePGNet(rule->isExceptSamePgnet());
        if (rule->getCutClass() != nullptr) {
          std::string className = rule->getCutClass()->getName();
          con->setCutClassName(className);
          auto cutClassIdx = tmpLayer->getCutClassIdx(className);
          if (cutClassIdx != -1) {
            con->setCutClassIdx(cutClassIdx);
          } else {
            continue;
          }
          con->setToAll(rule->isCutClassToAll());
        }
        con->setNoPrl(rule->isNoPrl());
        con->setSideParallelOverlap(rule->isSideParallelOverlap());
        con->setSameMask(rule->isSameMask());

        tmpLayer->addLef58CutSpacingConstraint(con.get());
        tech_->addUConstraint(std::move(con));
        break;
      }

      case odb::dbTechLayerCutSpacingRule::CutSpacingType::LAYER: {
        if (rule->getSecondLayer() == nullptr) {
          continue;
        }
        auto con = std::make_unique<frLef58CutSpacingConstraint>();
        con->setCutSpacing(rule->getCutSpacing());
        con->setCenterToCenter(rule->isCenterToCenter());
        con->setSameNet(rule->isSameNet());
        con->setSameMetal(rule->isSameMetal());
        con->setSameVia(rule->isSameVia());
        con->setSecondLayerName(rule->getSecondLayer()->getName());
        con->setStack(rule->isStack());
        if (rule->isOrthogonalSpacingValid()) {
          con->setOrthogonalSpacing(rule->getOrthogonalSpacing());
        }
        if (rule->getCutClass() != nullptr) {
          std::string className = rule->getCutClass()->getName();
          con->setCutClassName(className);
          auto cutClassIdx = tmpLayer->getCutClassIdx(className);
          if (cutClassIdx != -1) {
            con->setCutClassIdx(cutClassIdx);
          } else {
            continue;
          }
          con->setShortEdgeOnly(rule->isShortEdgeOnly());
          if (rule->isPrlValid()) {
            con->setPrl(rule->getPrl());
          }
          con->setConcaveCorner(rule->isConcaveCorner());
          if (rule->isConcaveCornerWidth()) {
            con->setWidth(rule->getWidth());
            con->setEnclosure(rule->getEnclosure());
            con->setEdgeLength(rule->getEdgeLength());
          } else if (rule->isConcaveCornerParallel()) {
            con->setParLength(rule->getParLength());
            con->setParWithin(rule->getParWithin());
            con->setEnclosure(rule->getParEnclosure());
          } else if (rule->isConcaveCornerEdgeLength()) {
            con->setEdgeLength(rule->getEdgeLength());
            con->setEdgeEnclosure(rule->getEdgeEnclosure());
            con->setAdjEnclosure(rule->getAdjEnclosure());
          }
          if (rule->isExtensionValid()) {
            con->setExtension(rule->getExtension());
          }
          if (rule->isNonEolConvexCorner()) {
            con->setEolWidth(rule->getEolWidth());
            if (rule->isMinLengthValid()) {
              con->setMinLength(rule->getMinLength());
            }
          }
          if (rule->isAboveWidthValid()) {
            rule->setWidth(rule->getAboveWidth());
            if (rule->isAboveWidthEnclosureValid()) {
              rule->setEnclosure(rule->getAboveEnclosure());
            }
          }
          con->setMaskOverlap(rule->isMaskOverlap());
          con->setWrongDirection(rule->isWrongDirection());
        }
        tmpLayer->addLef58CutSpacingConstraint(con.get());
        tech_->addUConstraint(std::move(con));
        break;
      }
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::AREA:
        logger_->warn(
            utl::DRT,
            258,
            "Unsupported LEF58_SPACING rule for layer {} of type AREA.",
            layer->getName());
        break;
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::MAXXY:
        logger_->warn(
            utl::DRT,
            161,
            "Unsupported LEF58_SPACING rule for layer {} of type MAXXY.",
            layer->getName());
        break;
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::SAMEMASK:
        logger_->warn(
            utl::DRT,
            259,
            "Unsupported LEF58_SPACING rule for layer {} of type SAMEMASK.",
            layer->getName());
        break;
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::PARALLELOVERLAP: {
        auto con = std::make_unique<frLef58CutSpacingConstraint>();
        con->setCutSpacing(rule->getCutSpacing());
        con->setCenterToCenter(rule->isCenterToCenter());
        con->setSameNet(rule->isSameNet());
        con->setSameMetal(rule->isSameMetal());
        con->setSameVia(rule->isSameVia());
        con->setSameCut(rule->isSameCut());
        con->setSameMask(rule->isSameMask());
        con->setParallelOverlap(true);
        tmpLayer->addLef58CutSpacingConstraint(con.get());
        tech_->addUConstraint(std::move(con));
        break;
      }
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::PARALLELWITHIN:
        logger_->warn(utl::DRT,
                      261,
                      "Unsupported LEF58_SPACING rule for layer {} of type "
                      "PARALLELWITHIN.",
                      layer->getName());
        break;
      case odb::dbTechLayerCutSpacingRule::CutSpacingType::SAMEMETALSHAREDEDGE:
        logger_->warn(utl::DRT,
                      262,
                      "Unsupported LEF58_SPACING rule for layer {} of type "
                      "SAMEMETALSHAREDEDGE.",
                      layer->getName());
        break;
      default:
        logger_->warn(utl::DRT,
                      263,
                      "Unsupported LEF58_SPACING rule for layer {}.",
                      layer->getName());
        break;
    }
  }
  for (auto rule : layer->getTechLayerCutSpacingTableDefRules()) {
    if (rule->isLayerValid() && tmpLayer->getLayerNum() == 1) {
      continue;
    }
    if (rule->isSameMask()) {
      logger_->warn(utl::DRT,
                    279,
                    "SAMEMASK unsupported for cut LEF58_SPACINGTABLE rule");
      continue;
    }
    auto con = std::make_unique<frLef58CutSpacingTableConstraint>(rule);
    frCollection<frCollection<std::pair<frCoord, frCoord>>> table;
    std::map<frString, frUInt4> rowMap, colMap;
    rule->getSpacingTable(table, rowMap, colMap);
    frString cutClass1 = colMap.begin()->first;
    if (cutClass1.find('/') != std::string::npos) {
      cutClass1 = cutClass1.substr(0, cutClass1.find('/'));
    }
    frString cutClass2 = rowMap.begin()->first;
    if (cutClass2.find('/') != std::string::npos) {
      cutClass2 = cutClass2.substr(0, cutClass2.find('/'));
    }
    auto spc = table[0][0];
    con->setDefaultSpacing(spc);
    con->setDefaultCenterToCenter(rule->isCenterToCenter(cutClass1, cutClass2));
    con->setDefaultCenterAndEdge(rule->isCenterAndEdge(cutClass1, cutClass2));
    if (rule->isLayerValid()) {
      if (rule->isSameMetal()) {
        tmpLayer->setLef58SameMetalInterCutSpcTblConstraint(con.get());
      } else if (rule->isSameNet()) {
        tmpLayer->setLef58SameNetInterCutSpcTblConstraint(con.get());
      } else {
        tmpLayer->setLef58DefaultInterCutSpcTblConstraint(con.get());
      }
    } else {
      if (rule->isSameMetal()) {
        tmpLayer->setLef58SameMetalCutSpcTblConstraint(con.get());
      } else if (rule->isSameNet()) {
        tmpLayer->setLef58SameNetCutSpcTblConstraint(con.get());
      } else {
        tmpLayer->setLef58DiffNetCutSpcTblConstraint(con.get());
      }
    }
    tech_->addUConstraint(std::move(con));
  }
  for (auto rule : layer->getTechLayerKeepOutZoneRules()) {
    if (rule->isSameMask()) {
      logger_->warn(
          DRT,
          324,
          "LEF58_KEEPOUTZONE SAMEMASK is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->isSameMetal()) {
      logger_->warn(
          DRT,
          325,
          "LEF58_KEEPOUTZONE SAMEMETAL is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->isDiffMetal()) {
      logger_->warn(
          DRT,
          326,
          "LEF58_KEEPOUTZONE DIFFMETAL is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->getSideExtension() > 0 || rule->getForwardExtension() > 0) {
      logger_->warn(
          DRT,
          327,
          "LEF58_KEEPOUTZONE EXTENSION is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->getSpiralExtension() > 0) {
      logger_->warn(DRT,
                    328,
                    "LEF58_KEEPOUTZONE non zero SPIRALEXTENSION is not "
                    "supported. Skipping for layer {}",
                    layer->getName());
      continue;
    }
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frLef58KeepOutZoneConstraint>(rule);
    auto rptr = static_cast<frLef58KeepOutZoneConstraint*>(uCon.get());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addKeepOutZoneConstraint(rptr);
  }
  for (auto rule : layer->getTechLayerCutEnclosureRules()) {
    if (rule->getType() == odb::dbTechLayerCutEnclosureRule::EOL) {
      logger_->warn(
          DRT,
          340,
          "LEF58_ENCLOSURE EOL is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->getType() == odb::dbTechLayerCutEnclosureRule::HORZ_AND_VERT) {
      logger_->warn(DRT,
                    341,
                    "LEF58_ENCLOSURE HORIZONTAL/VERTICAL is not supported. "
                    "Skipping for layer {}",
                    layer->getName());
      continue;
    }
    if (rule->isIncludeAbutted()) {
      logger_->warn(DRT,
                    342,
                    "LEF58_ENCLOSURE INCLUDEABUTTED is not supported. Skipping "
                    "for layer {}",
                    layer->getName());
      continue;
    }
    if (rule->isOffCenterLine()) {
      logger_->warn(DRT,
                    343,
                    "LEF58_ENCLOSURE OFFCENTERLINE is not supported. Skipping "
                    "for layer {}",
                    layer->getName());
      continue;
    }
    if (rule->isLengthValid()) {
      logger_->warn(
          DRT,
          344,
          "LEF58_ENCLOSURE LENGTH is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->isExtraCutValid()) {
      logger_->warn(
          DRT,
          345,
          "LEF58_ENCLOSURE EXTRACUT is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->isRedundantCutValid()) {
      logger_->warn(DRT,
                    346,
                    "LEF58_ENCLOSURE REDUNDANTCUT is not supported. Skipping "
                    "for layer {}",
                    layer->getName());
      continue;
    }
    if (rule->isParallelValid()) {
      logger_->warn(
          DRT,
          347,
          "LEF58_ENCLOSURE PARALLEL is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->isConcaveCornersValid()) {
      logger_->warn(DRT,
                    348,
                    "LEF58_ENCLOSURE CONCAVECORNERS is not supported. Skipping "
                    "for layer {}",
                    layer->getName());
      continue;
    }
    if (!rule->isCutClassValid()) {
      logger_->warn(DRT,
                    349,
                    "LEF58_ENCLOSURE with no CUTCLASS is not supported. "
                    "Skipping for layer {}",
                    layer->getName());
      continue;
    }
    if (!rule->isWidthValid()) {
      logger_->warn(DRT,
                    350,
                    "LEF58_ENCLOSURE with no WIDTH is not supported. Skipping "
                    "for layer {}",
                    layer->getName());
      continue;
    }
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frLef58EnclosureConstraint>(rule);
    auto rptr = static_cast<frLef58EnclosureConstraint*>(uCon.get());
    rptr->setCutClassIdx(
        tmpLayer->getCutClassIdx(rule->getCutClass()->getName()));
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addLef58EnclosureConstraint(rptr);
  }
}

void io::Parser::addDefaultMasterSliceLayer()
{
  std::unique_ptr<frLayer> uMSLayer = std::make_unique<frLayer>();
  auto tmpMSLayer = uMSLayer.get();
  if (masterSliceLayer_ == nullptr) {
    tmpMSLayer->setFakeMasterslice(true);
  } else {
    tmpMSLayer->setDbLayer(masterSliceLayer_);
  }
  tmpMSLayer->setLayerNum(readLayerCnt_++);
  tech_->addLayer(std::move(uMSLayer));
}

void io::Parser::addDefaultCutLayer()
{
  std::unique_ptr<frLayer> uCutLayer = std::make_unique<frLayer>();
  auto tmpCutLayer = uCutLayer.get();
  tmpCutLayer->setFakeCut(true);
  tmpCutLayer->setLayerNum(readLayerCnt_++);
  tech_->addLayer(std::move(uCutLayer));
}

void io::Parser::addRoutingLayer(odb::dbTechLayer* layer)
{
  if (layer->getRoutingLevel() == 0) {
    return;
  }
  if (readLayerCnt_ == 0) {
    addDefaultMasterSliceLayer();
    addDefaultCutLayer();
  }
  std::unique_ptr<frLayer> uLayer = std::make_unique<frLayer>();
  auto tmpLayer = uLayer.get();
  tmpLayer->setDbLayer(layer);
  tmpLayer->setLayerNum(readLayerCnt_++);
  tech_->addLayer(std::move(uLayer));

  tmpLayer->setWidth(layer->getWidth());
  if (layer->getMinWidth() > layer->getWidth()) {
    logger_->warn(
        DRT,
        210,
        "Layer {} minWidth is larger than width. Using width as minWidth.",
        layer->getName());
  }
  tmpLayer->setMinWidth(std::min(layer->getMinWidth(), layer->getWidth()));
  // add minWidth constraint
  auto minWidthConstraint
      = std::make_unique<frMinWidthConstraint>(tmpLayer->getMinWidth());
  tmpLayer->setMinWidthConstraint(minWidthConstraint.get());
  tech_->addUConstraint(std::move(minWidthConstraint));

  // Add off grid rule for every layer
  auto recheckConstraint = std::make_unique<frRecheckConstraint>();
  tmpLayer->setRecheckConstraint(recheckConstraint.get());
  tech_->addUConstraint(std::move(recheckConstraint));

  // Add short rule for every layer
  auto shortConstraint = std::make_unique<frShortConstraint>();
  tmpLayer->setShortConstraint(shortConstraint.get());
  tech_->addUConstraint(std::move(shortConstraint));

  // Add off grid rule for every layer
  auto offGridConstraint = std::make_unique<frOffGridConstraint>();
  tmpLayer->setOffGridConstraint(offGridConstraint.get());
  tech_->addUConstraint(std::move(offGridConstraint));

  // Add nsmetal rule for every layer
  auto nsmetalConstraint = std::make_unique<frNonSufficientMetalConstraint>();
  tmpLayer->setNonSufficientMetalConstraint(nsmetalConstraint.get());

  tech_->addUConstraint(std::move(nsmetalConstraint));
  setRoutingLayerProperties(layer, tmpLayer);
  // read minArea rule
  if (layer->hasArea()) {
    frCoord minArea = frCoord(
        round(layer->getArea() * tech_->getDBUPerUU() * tech_->getDBUPerUU()));
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frAreaConstraint>(minArea);
    auto rptr = static_cast<frAreaConstraint*>(uCon.get());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->setAreaConstraint(rptr);
  }

  if (layer->hasMinStep()) {
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frMinStepConstraint>();
    auto rptr = static_cast<frMinStepConstraint*>(uCon.get());
    rptr->setInsideCorner(layer->getMinStepType()
                          == odb::dbTechLayerMinStepType::INSIDE_CORNER);
    rptr->setOutsideCorner(layer->getMinStepType()
                           == odb::dbTechLayerMinStepType::OUTSIDE_CORNER);
    rptr->setStep(layer->getMinStepType() == odb::dbTechLayerMinStepType::STEP);
    switch (layer->getMinStepType()) {
      case odb::dbTechLayerMinStepType::INSIDE_CORNER:
        rptr->setMinstepType(frMinstepTypeEnum::INSIDECORNER);
        break;
      case odb::dbTechLayerMinStepType::OUTSIDE_CORNER:
        rptr->setMinstepType(frMinstepTypeEnum::OUTSIDECORNER);
        break;
      case odb::dbTechLayerMinStepType::STEP:
        rptr->setMinstepType(frMinstepTypeEnum::STEP);
        break;
      default:
        break;
    }
    if (layer->hasMinStepMaxLength()) {
      rptr->setMaxLength(layer->getMinStepMaxLength());
    }
    if (layer->hasMinStepMaxEdges()) {
      rptr->setMaxEdges(layer->getMinStepMaxEdges());
      rptr->setInsideCorner(true);
      rptr->setOutsideCorner(true);
      rptr->setStep(true);
      rptr->setMinstepType(frMinstepTypeEnum::UNKNOWN);
    }
    rptr->setMinStepLength(layer->getMinStep());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->setMinStepConstraint(rptr);
  }

  // read minHole rule
  std::vector<odb::dbTechMinEncRule*> minEncRules;
  layer->getMinEnclosureRules(minEncRules);
  for (odb::dbTechMinEncRule* rule : minEncRules) {
    frUInt4 _minEnclosedWidth = -1;
    bool hasMinenclosedareaWidth = rule->getEnclosureWidth(_minEnclosedWidth);
    if (hasMinenclosedareaWidth) {
      logger_->warn(
          DRT,
          139,
          "minEnclosedArea constraint with width is not supported, skipped.");
      continue;
    }
    frUInt4 _minEnclosedArea;
    rule->getEnclosure(_minEnclosedArea);
    frCoord minEnclosedArea = _minEnclosedArea;
    auto minEnclosedAreaConstraint
        = std::make_unique<frMinEnclosedAreaConstraint>(minEnclosedArea);
    tmpLayer->addMinEnclosedAreaConstraint(minEnclosedAreaConstraint.get());
    tech_->addUConstraint(std::move(minEnclosedAreaConstraint));
  }

  // read spacing rule
  for (auto rule : layer->getV54SpacingRules()) {
    frCoord minSpacing = rule->getSpacing();
    frUInt4 _eolWidth = 0, _eolWithin = 0, _parSpace = 0, _parWithin = 0;
    bool hasSpacingParellelEdge = false;
    bool hasSpacingTwoEdges = false;
    bool hasSpacingEndOfLine = rule->getEol(_eolWidth,
                                            _eolWithin,
                                            hasSpacingParellelEdge,
                                            _parSpace,
                                            _parWithin,
                                            hasSpacingTwoEdges);
    frCoord eolWidth(_eolWidth), eolWithin(_eolWithin), parSpace(_parSpace),
        parWithin(_parWithin);
    if (rule->hasRange()) {
      std::unique_ptr<frSpacingRangeConstraint> uCon
          = std::make_unique<frSpacingRangeConstraint>();
      uCon->setMinSpacing(minSpacing);
      frUInt4 minWidth, maxWidth;
      rule->getRange(minWidth, maxWidth);
      uCon->setMinWidth(minWidth);
      uCon->setMaxWidth(maxWidth);
      uCon->setLayer(tmpLayer);
      tmpLayer->addSpacingRangeConstraint(uCon.get());
      tech_->addUConstraint(std::move(uCon));
    } else if (rule->hasLengthThreshold()) {
      logger_->warn(DRT, 141, "SpacingLengthThreshold unsupported.");
    } else if (rule->hasSpacingNotchLength()) {
      logger_->warn(DRT, 142, "SpacingNotchLength unsupported.");
    } else if (rule->hasSpacingEndOfNotchWidth()) {
      logger_->warn(DRT, 143, "SpacingEndOfNotchWidth unsupported.");
    } else if (hasSpacingEndOfLine) {
      std::unique_ptr<frConstraint> uCon
          = std::make_unique<frSpacingEndOfLineConstraint>();
      auto rptr = static_cast<frSpacingEndOfLineConstraint*>(uCon.get());
      rptr->setMinSpacing(minSpacing);
      rptr->setEolWidth(eolWidth);
      rptr->setEolWithin(eolWithin);
      if (hasSpacingParellelEdge) {
        rptr->setParSpace(parSpace);
        rptr->setParWithin(parWithin);
        rptr->setTwoEdges(hasSpacingTwoEdges);
      }
      tech_->addUConstraint(std::move(uCon));
      tmpLayer->addEolSpacing(rptr);
    } else if (rule->getCutSameNet()) {
      bool pgOnly = rule->getSameNetPgOnly();
      std::unique_ptr<frConstraint> uCon
          = std::make_unique<frSpacingSamenetConstraint>(minSpacing, pgOnly);
      auto rptr = uCon.get();
      tech_->addUConstraint(std::move(uCon));
      tmpLayer->setSpacingSamenet(
          static_cast<frSpacingSamenetConstraint*>(rptr));
    } else {
      frCollection<frCoord> rowVals(1, 0), colVals(1, 0);
      frCollection<frCollection<frCoord>> tblVals(1, {minSpacing});
      frString rowName("WIDTH"), colName("PARALLELRUNLENGTH");
      std::unique_ptr<frConstraint> uCon
          = std::make_unique<frSpacingTablePrlConstraint>(
              fr2DLookupTbl(rowName, rowVals, colName, colVals, tblVals));
      auto rptr = static_cast<frSpacingTablePrlConstraint*>(uCon.get());
      tech_->addUConstraint(std::move(uCon));
      tmpLayer->setMinSpacing(rptr);
    }
  }
  if (!layer->getV55InfluenceRules().empty()) {
    frCollection<frCoord> widthTbl;
    frCollection<std::pair<frCoord, frCoord>> valTbl;
    for (auto rule : layer->getV55InfluenceRules()) {
      frUInt4 width, within, spacing;
      rule->getV55InfluenceEntry(width, within, spacing);
      widthTbl.push_back(width);
      valTbl.push_back({within, spacing});
    }
    fr1DLookupTbl<frCoord, std::pair<frCoord, frCoord>> tbl(
        "WIDTH", widthTbl, valTbl);
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frSpacingTableInfluenceConstraint>(tbl);
    auto rptr = static_cast<frSpacingTableInfluenceConstraint*>(uCon.get());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->setSpacingTableInfluence(rptr);
  }
  // read prl spacingTable
  if (layer->hasV55SpacingRules()) {
    frCollection<frUInt4> _rowVals, _colVals;
    frCollection<frCollection<frUInt4>> _tblVals;
    layer->getV55SpacingWidthsAndLengths(_rowVals, _colVals);
    layer->getV55SpacingTable(_tblVals);
    frCollection<frCoord> rowVals(_rowVals.begin(), _rowVals.end());
    frCollection<frCoord> colVals(_colVals.begin(), _colVals.end());
    frCollection<frCollection<frCoord>> tblVals;
    tblVals.resize(_tblVals.size());
    for (size_t i = 0; i < _tblVals.size(); i++) {
      for (size_t j = 0; j < _tblVals[i].size(); j++) {
        tblVals[i].push_back(_tblVals[i][j]);
      }
    }

    std::unique_ptr<frSpacingTableConstraint> spacingTableConstraint;
    std::shared_ptr<fr2DLookupTbl<frCoord, frCoord, frCoord>> prlTbl;
    frString rowName("WIDTH"), colName("PARALLELRUNLENGTH");

    // old
    prlTbl = std::make_shared<fr2DLookupTbl<frCoord, frCoord, frCoord>>(
        rowName, rowVals, colName, colVals, tblVals);
    spacingTableConstraint = std::make_unique<frSpacingTableConstraint>(prlTbl);
    tmpLayer->addConstraint(spacingTableConstraint.get());
    tech_->addUConstraint(std::move(spacingTableConstraint));
    // new
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frSpacingTablePrlConstraint>(
            fr2DLookupTbl(rowName, rowVals, colName, colVals, tblVals));
    auto rptr = static_cast<frSpacingTablePrlConstraint*>(uCon.get());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->setMinSpacing(rptr);
  }

  if (layer->hasTwoWidthsSpacingRules()) {
    frCollection<frCollection<frUInt4>> _tblVals;
    layer->getTwoWidthsSpacingTable(_tblVals);
    frCollection<frCollection<frCoord>> tblVals;
    tblVals.resize(_tblVals.size());
    for (size_t i = 0; i < _tblVals.size(); i++) {
      for (size_t j = 0; j < _tblVals[i].size(); j++) {
        tblVals[i].push_back(_tblVals[i][j]);
      }
    }

    frCollection<frSpacingTableTwRowType> rowVals;
    for (uint j = 0; j < layer->getTwoWidthsSpacingTableNumWidths(); ++j) {
      frCoord width = layer->getTwoWidthsSpacingTableWidth(j);
      frCoord prl = layer->getTwoWidthsSpacingTablePRL(j);
      rowVals.push_back(frSpacingTableTwRowType(width, prl));
    }

    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frSpacingTableTwConstraint>(rowVals, tblVals);
    auto rptr = static_cast<frSpacingTableTwConstraint*>(uCon.get());
    rptr->setLayer(tmpLayer);
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->setMinSpacing(rptr);
  }

  for (auto rule : layer->getMinCutRules()) {
    frUInt4 numCuts, width, within, length, distance;
    if (!rule->getMinimumCuts(numCuts, width)) {
      continue;
    }
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frMinimumcutConstraint>();
    auto rptr = static_cast<frMinimumcutConstraint*>(uCon.get());
    rptr->setNumCuts(numCuts);
    rptr->setWidth(width);
    if (rule->getCutDistance(within)) {
      rptr->setWithin(within);
    }
    if (rule->isAboveOnly()) {
      rptr->setConnection(frMinimumcutConnectionEnum::FROMABOVE);
    }
    if (rule->isBelowOnly()) {
      rptr->setConnection(frMinimumcutConnectionEnum::FROMBELOW);
    }
    if (rule->getLengthForCuts(length, distance)) {
      rptr->setLength(length, distance);
    }
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addMinimumcutConstraint(rptr);
  }
  for (auto rule : layer->getTechLayerMinCutRules()) {
    if (rule->isAreaValid()) {
      logger_->warn(
          DRT,
          317,
          "LEF58_MINIMUMCUT AREA is not supported. Skipping for layer {}",
          layer->getName());
      continue;
    }
    if (rule->isSameMetalOverlap()) {
      logger_->warn(DRT,
                    318,
                    "LEF58_MINIMUMCUT SAMEMETALOVERLAP is not supported. "
                    "Skipping for layer {}",
                    layer->getName());
      continue;
    }
    if (rule->isFullyEnclosed()) {
      logger_->warn(DRT,
                    319,
                    "LEF58_MINIMUMCUT FULLYENCLOSED is not supported. Skipping "
                    "for layer {}",
                    layer->getName());
      continue;
    }
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frLef58MinimumcutConstraint>(rule);
    auto rptr = static_cast<frLef58MinimumcutConstraint*>(uCon.get());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addLef58MinimumcutConstraint(rptr);
  }

  for (auto rule : layer->getTechLayerEolKeepOutRules()) {
    std::unique_ptr<frConstraint> uCon
        = std::make_unique<frLef58EolKeepOutConstraint>();
    auto rptr = static_cast<frLef58EolKeepOutConstraint*>(uCon.get());
    rptr->setEolWidth(rule->getEolWidth());
    rptr->setBackwardExt(rule->getBackwardExt());
    rptr->setForwardExt(rule->getForwardExt());
    rptr->setSideExt(rule->getSideExt());
    rptr->setCornerOnly(rule->isCornerOnly());
    rptr->setExceptWithin(rule->isExceptWithin());
    rptr->setWithinLow(rule->getWithinLow());
    rptr->setWithinHigh(rule->getWithinHigh());
    tech_->addUConstraint(std::move(uCon));
    tmpLayer->addLef58EolKeepOutConstraint(rptr);
  }
}

void io::Parser::addCutLayer(odb::dbTechLayer* layer)
{
  if (layer->getLef58Type() == odb::dbTechLayer::LEF58_TYPE::MIMCAP) {
    return;
  }
  if (readLayerCnt_ == 0) {
    addDefaultMasterSliceLayer();
  }
  auto* lowerlayer = layer->getLowerLayer();
  if (lowerlayer != nullptr
      && lowerlayer->getType() == odb::dbTechLayerType::CUT) {
    return;
  }

  std::unique_ptr<frLayer> uLayer = std::make_unique<frLayer>();
  auto tmpLayer = uLayer.get();
  tmpLayer->setDbLayer(layer);
  tmpLayer->setLayerNum(readLayerCnt_++);
  tech_->addLayer(std::move(uLayer));

  auto shortConstraint = std::make_unique<frShortConstraint>();
  tmpLayer->addConstraint(shortConstraint.get());
  tmpLayer->setShortConstraint(shortConstraint.get());
  tech_->addUConstraint(std::move(shortConstraint));

  // read spacing constraint
  for (odb::dbTechLayerSpacingRule* rule : layer->getV54SpacingRules()) {
    std::unique_ptr<frCutSpacingConstraint> cutSpacingConstraint;
    frCoord cutArea = rule->getCutArea();
    frCoord cutSpacing = rule->getSpacing();
    bool centerToCenter = rule->getCutCenterToCenter();
    bool sameNet = rule->getCutSameNet();
    bool stack = rule->getCutStacking();
    bool exceptSamePGNet = rule->getSameNetPgOnly();
    bool parallelOverlap = rule->getCutParallelOverlap();
    odb::dbTechLayer* outly;
    frString secondLayerName = std::string("");
    if (rule->getCutLayer4Spacing(outly)) {
      secondLayerName = std::string(outly->getName());
    }
    frUInt4 _adjacentCuts;
    frUInt4 within;
    frUInt4 spacing;
    bool except_same_pgnet;
    frCoord cutWithin = 0;
    int adjacentCuts = 0;
    if (rule->getAdjacentCuts(
            _adjacentCuts, within, spacing, except_same_pgnet)) {
      adjacentCuts = _adjacentCuts;
      cutWithin = within;
    }

    // initialize for invalid variables
    cutArea = (cutArea == 0) ? -1 : cutArea;
    cutWithin = (cutWithin == 0) ? -1 : cutWithin;
    adjacentCuts = (adjacentCuts == 0) ? -1 : adjacentCuts;

    if (cutWithin != -1 && cutWithin < cutSpacing) {
      logger_->warn(DRT,
                    147,
                    "cutWithin is smaller than cutSpacing for ADJACENTCUTS on "
                    "layer {}, please check your rule definition.",
                    layer->getName());
    }
    cutSpacingConstraint
        = std::make_unique<frCutSpacingConstraint>(cutSpacing,
                                                   centerToCenter,
                                                   sameNet,
                                                   secondLayerName,
                                                   stack,
                                                   adjacentCuts,
                                                   cutWithin,
                                                   exceptSamePGNet,
                                                   parallelOverlap,
                                                   cutArea);
    tmpLayer->addCutSpacingConstraint(cutSpacingConstraint.get());
    tech_->addUConstraint(std::move(cutSpacingConstraint));
  }

  // lef58
  setCutLayerProperties(layer, tmpLayer);
}

void io::Parser::addMasterSliceLayer(odb::dbTechLayer* layer)
{
  if (layer->getLef58Type() != odb::dbTechLayer::LEF58_TYPE::NWELL
      && layer->getLef58Type() != odb::dbTechLayer::LEF58_TYPE::PWELL
      && layer->getLef58Type() != odb::dbTechLayer::LEF58_TYPE::DIFFUSION) {
    masterSliceLayer_ = layer;
  }
}

void io::Parser::setLayers(odb::dbTech* db_tech)
{
  masterSliceLayer_ = nullptr;
  for (auto layer : db_tech->getLayers()) {
    switch (layer->getType().getValue()) {
      case odb::dbTechLayerType::ROUTING:
        addRoutingLayer(layer);
        break;
      case odb::dbTechLayerType::CUT:
        addCutLayer(layer);
        break;
      case odb::dbTechLayerType::MASTERSLICE:
        addMasterSliceLayer(layer);
        break;
      default:
        break;
    }
  }
  // MetalWidthViaMap
  for (auto rule : db_tech->getMetalWidthViaMap()) {
    auto db_layer = rule->getCutLayer();
    auto layer = tech_->getLayer(db_layer->getName());
    if (layer == nullptr) {
      continue;
    }
    auto uCon = std::make_unique<frMetalWidthViaConstraint>(rule);
    layer->addMetalWidthViaConstraint(uCon.get());
    tech_->addUConstraint(std::move(uCon));
  }
}

void io::Parser::setMasters(odb::dbDatabase* db)
{
  const frLayerNum numLayers = tech_->getLayers().size();
  std::vector<RTree<frMPin*>> pin_shapes;
  pin_shapes.resize(numLayers);
  auto addPinFig
      = [&pin_shapes](const Rect& box, frLayerNum lNum, frMPin* pinIn) {
          std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
          pinFig->setBBox(box);
          pinFig->addToPin(pinIn);
          pinFig->setLayerNum(lNum);
          std::unique_ptr<frPinFig> uptr(std::move(pinFig));
          pinIn->addPinFig(std::move(uptr));
          pin_shapes[lNum].insert(std::make_pair(box, pinIn));
        };

  for (auto lib : db->getLibs()) {
    for (odb::dbMaster* master : lib->getMasters()) {
      for (auto& tree : pin_shapes) {
        tree.clear();
      }
      auto tmpMaster = std::make_unique<frMaster>(master->getName());
      odb::Point origin = master->getOrigin();
      frCoord sizeX = master->getWidth();
      frCoord sizeY = master->getHeight();
      std::vector<frBoundary> bounds;
      frBoundary bound;
      std::vector<Point> points;
      points.emplace_back(origin);
      points.emplace_back(sizeX, origin.y());
      points.emplace_back(sizeX, sizeY);
      points.emplace_back(origin.x(), sizeY);
      bound.setPoints(points);
      bounds.emplace_back(bound);
      tmpMaster->setBoundaries(bounds);
      tmpMaster->setMasterType(master->getType());

      for (auto _term : master->getMTerms()) {
        std::unique_ptr<frMTerm> uTerm
            = std::make_unique<frMTerm>(_term->getName());
        auto term = uTerm.get();
        term->setId(numTerms_);
        numTerms_++;
        tmpMaster->addTerm(std::move(uTerm));

        term->setType(_term->getSigType());
        term->setDirection(_term->getIoType());

        int i = 0;
        for (auto mpin : _term->getMPins()) {
          auto pinIn = std::make_unique<frMPin>();
          pinIn->setId(i++);
          for (auto box : mpin->getGeometry()) {
            auto layer = box->getTechLayer();
            if (!layer) {
              if (tech_->name2via_.find(box->getTechVia()->getName())
                  == tech_->name2via_.end()) {
                logger_->warn(DRT,
                              193,
                              "Skipping unsupported via {} in macro pin {}/{}.",
                              box->getTechVia()->getName(),
                              master->getName(),
                              _term->getName());
              }
              int x, y;
              box->getViaXY(x, y);
              auto viaDef = tech_->name2via_[box->getTechVia()->getName()];
              auto tmpP = std::make_unique<frVia>(viaDef);
              tmpP->setOrigin({x, y});
              // layer1 rect
              addPinFig(
                  tmpP->getLayer1BBox(), viaDef->getLayer1Num(), pinIn.get());
              // layer2 rect
              addPinFig(
                  tmpP->getLayer2BBox(), viaDef->getLayer2Num(), pinIn.get());
              // cut rect
              addPinFig(
                  tmpP->getCutBBox(), viaDef->getCutLayerNum(), pinIn.get());
            } else {
              std::string layer_name = layer->getName();
              if (tech_->name2layer_.find(layer_name)
                  == tech_->name2layer_.end()) {
                auto type = box->getTechLayer()->getType();
                if (type == odb::dbTechLayerType::ROUTING
                    || type == odb::dbTechLayerType::CUT) {
                  logger_->warn(DRT,
                                122,
                                "Layer {} is skipped for {}/{}.",
                                layer_name,
                                tmpMaster->getName(),
                                _term->getName());
                }
                continue;
              }
              frLayerNum layerNum
                  = tech_->name2layer_.at(layer_name)->getLayerNum();

              frCoord xl = box->xMin();
              frCoord yl = box->yMin();
              frCoord xh = box->xMax();
              frCoord yh = box->yMax();
              addPinFig(Rect(xl, yl, xh, yh), layerNum, pinIn.get());
            }
          }
          term->addPin(std::move(pinIn));
        }
      }

      std::vector<gtl::polygon_90_set_data<frCoord>> layerPolys(
          tech_->getLayers().size());
      for (auto obs : master->getObstructions()) {
        frLayerNum layerNum = -1;
        auto layer = obs->getTechLayer();
        std::string layer_name = layer->getName();
        auto layer_type = layer->getType();
        if (tech_->name2layer_.find(layer_name) == tech_->name2layer_.end()) {
          if (layer_type == odb::dbTechLayerType::ROUTING
              || layer_type == odb::dbTechLayerType::CUT) {
            logger_->warn(DRT,
                          123,
                          "Layer {} is skipped for {}/OBS.",
                          layer_name,
                          tmpMaster->getName());
          }
          continue;
        }
        layerNum = tech_->name2layer_.at(layer_name)->getLayerNum();

        frCoord xl = obs->xMin();
        frCoord yl = obs->yMin();
        frCoord xh = obs->xMax();
        frCoord yh = obs->yMax();

        // In some LEF they put contact cut shapes in the pin and in others
        // they put them in OBS for some unknown reason.  The later confuses gc
        // into thinking the cuts are diff-net.  To resolve this we move
        // any cut OBS enclosed by a pin shape into the pin.
        if (layer_type == odb::dbTechLayerType::CUT) {
          std::vector<rq_box_value_t<frMPin*>> containing_pins;
          if (layerNum + 1 < pin_shapes.size()) {
            pin_shapes[layerNum + 1].query(
                bgi::intersects(Rect{xl, yl, xh, yh}),
                back_inserter(containing_pins));
          }
          if (!containing_pins.empty()) {
            frMPin* pin = nullptr;
            for (auto& [box, rqPin] : containing_pins) {
              if (!pin) {
                pin = rqPin;
              } else if (pin != rqPin) {
                pin = nullptr;
                break;  // skip if more than one pin
              }
            }
            if (pin) {
              std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
              pinFig->setBBox(Rect(xl, yl, xh, yh));
              pinFig->addToPin(pin);
              pinFig->setLayerNum(layerNum);
              std::unique_ptr<frPinFig> uptr(std::move(pinFig));
              pin->addPinFig(std::move(uptr));
              continue;
            }
          }
        }
        if (obs->getDesignRuleWidth() == -1) {
          gtl::rectangle_data<frCoord> rect(xl, yl, xh, yh);
          using gtl::operators::operator+=;
          layerPolys[layerNum] += rect;
        } else {
          auto blkIn = std::make_unique<frBlockage>();
          blkIn->setId(numBlockages_++);
          blkIn->setDesignRuleWidth(obs->getDesignRuleWidth());
          auto pinIn = std::make_unique<frBPin>();
          pinIn->setId(0);
          // pinFig
          std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
          pinFig->setBBox(Rect(xl, yl, xh, yh));
          pinFig->addToPin(pinIn.get());
          pinFig->setLayerNum(layerNum);
          std::unique_ptr<frPinFig> uptr(std::move(pinFig));
          pinIn->addPinFig(std::move(uptr));
          blkIn->setPin(std::move(pinIn));
          tmpMaster->addBlockage(std::move(blkIn));
        }
      }
      frLayerNum lNum = 0;
      for (auto& polySet : layerPolys) {
        std::vector<gtl::polygon_90_with_holes_data<frCoord>> polys;
        polySet.get(polys);
        for (auto& poly : polys) {
          std::vector<gtl::rectangle_data<frCoord>> rects;
          gtl::get_max_rectangles(rects, poly);
          for (auto& rect : rects) {
            frCoord xl = gtl::xl(rect);
            frCoord yl = gtl::yl(rect);
            frCoord xh = gtl::xh(rect);
            frCoord yh = gtl::yh(rect);
            auto blkIn = std::make_unique<frBlockage>();
            blkIn->setId(numBlockages_);
            numBlockages_++;
            auto pinIn = std::make_unique<frBPin>();
            pinIn->setId(0);
            // pinFig
            std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
            pinFig->setBBox(Rect(xl, yl, xh, yh));
            pinFig->addToPin(pinIn.get());
            pinFig->setLayerNum(lNum);
            std::unique_ptr<frPinFig> uptr(std::move(pinFig));
            pinIn->addPinFig(std::move(uptr));
            blkIn->setPin(std::move(pinIn));
            tmpMaster->addBlockage(std::move(blkIn));
          }
        }
        lNum++;
      }
      tmpMaster->setId(numMasters_ + 1);
      design_->addMaster(std::move(tmpMaster));
      numMasters_++;
      numTerms_ = 0;
      numBlockages_ = 0;
    }
  }
}

void io::Parser::setTechViaRules(odb::dbTech* db_tech)
{
  for (auto rule : db_tech->getViaGenerateRules()) {
    int count = rule->getViaLayerRuleCount();
    if (count != 3) {
      logger_->error(DRT, 128, "Unsupported viarule {}.", rule->getName());
    }
    std::map<frLayerNum, int> lNum2Int;
    for (int i = 0; i < count; i++) {
      auto layerRule = rule->getViaLayerRule(i);
      std::string layerName = layerRule->getLayer()->getName();
      if (tech_->name2layer_.find(layerName) == tech_->name2layer_.end()) {
        logger_->error(DRT,
                       129,
                       "Unknown layer {} for viarule {}.",
                       layerName,
                       rule->getName());
      }
      frLayerNum lNum = tech_->name2layer_[layerName]->getLayerNum();
      lNum2Int[lNum] = 1;
    }
    int curOrder = 0;
    for (auto [lnum, i] : lNum2Int) {
      lNum2Int[lnum] = ++curOrder;
    }
    if (lNum2Int.begin()->first + count - 1 != (--lNum2Int.end())->first) {
      logger_->error(
          DRT, 130, "Non-consecutive layers for viarule {}.", rule->getName());
    }
    auto viaRuleGen = std::make_unique<frViaRuleGenerate>(rule->getName());
    if (rule->isDefault()) {
      viaRuleGen->setDefault(true);
    }
    for (int i = 0; i < count; i++) {
      auto layerRule = rule->getViaLayerRule(i);
      frLayerNum layerNum
          = tech_->name2layer_[layerRule->getLayer()->getName()]->getLayerNum();
      if (layerRule->hasEnclosure()) {
        frCoord x;
        frCoord y;
        layerRule->getEnclosure(x, y);
        Point enc(x, y);
        switch (lNum2Int[layerNum]) {
          case 1:
            viaRuleGen->setLayer1Enc(enc);
            break;
          case 2:
            logger_->warn(DRT,
                          131,
                          "cutLayer cannot have overhangs in viarule {}, "
                          "skipping enclosure.",
                          rule->getName());
            break;
          default:
            viaRuleGen->setLayer2Enc(enc);
            break;
        }
      }
      if (layerRule->hasRect()) {
        odb::Rect rect;
        layerRule->getRect(rect);
        frCoord xl = rect.xMin();
        frCoord yl = rect.yMin();
        frCoord xh = rect.xMax();
        frCoord yh = rect.yMax();
        Rect box(xl, yl, xh, yh);
        switch (lNum2Int[layerNum]) {
          case 1:
            logger_->warn(
                DRT,
                132,
                "botLayer cannot have rect in viarule {}, skipping rect.",
                rule->getName());
            break;
          case 2:
            viaRuleGen->setCutRect(box);
            break;
          default:
            logger_->warn(
                DRT,
                133,
                "topLayer cannot have rect in viarule {}, skipping rect.",
                rule->getName());
            break;
        }
      }
      if (layerRule->hasSpacing()) {
        frCoord x;
        frCoord y;
        layerRule->getSpacing(x, y);
        Point pt(x, y);
        switch (lNum2Int[layerNum]) {
          case 1:
            logger_->warn(
                DRT,
                134,
                "botLayer cannot have spacing in viarule {}, skipping spacing.",
                rule->getName());
            break;
          case 2:
            viaRuleGen->setCutSpacing(pt);
            break;
          default:
            logger_->warn(
                DRT,
                135,
                "botLayer cannot have spacing in viarule {}, skipping spacing.",
                rule->getName());
            break;
        }
      }
    }
    tech_->addViaRuleGenerate(std::move(viaRuleGen));
  }
}

void io::Parser::setTechVias(odb::dbTech* db_tech)
{
  for (auto via : db_tech->getVias()) {
    std::map<frLayerNum, int> lNum2Int;
    bool has_unknown_layer = false;
    for (auto box : via->getBoxes()) {
      std::string layerName = box->getTechLayer()->getName();
      if (tech_->name2layer_.find(layerName) == tech_->name2layer_.end()) {
        logger_->warn(DRT,
                      124,
                      "Via {} with unused layer {} will be ignored.",
                      layerName,
                      via->getName());
        has_unknown_layer = true;
        continue;
      }
      if (box->getTechLayer()->getType() == dbTechLayerType::MASTERSLICE) {
        logger_->warn(DRT,
                      192,
                      "Via {} with MASTERSLICE layer {} will be ignored.",
                      layerName,
                      via->getName());
        has_unknown_layer = true;
        continue;
      }
      frLayerNum lNum = tech_->name2layer_[layerName]->getLayerNum();
      lNum2Int[lNum] = 1;
    }
    if (has_unknown_layer) {
      continue;
    }
    if (lNum2Int.size() != 3) {
      logger_->error(DRT, 125, "Unsupported via {}.", via->getName());
    }
    int curOrder = 0;
    for (auto [lnum, i] : lNum2Int) {
      lNum2Int[lnum] = ++curOrder;
    }

    if (lNum2Int.begin()->first + 2 != (--lNum2Int.end())->first) {
      logger_->error(
          DRT, 126, "Non-consecutive layers for via {}.", via->getName());
    }
    auto viaDef = std::make_unique<frViaDef>(via->getName());
    if (via->isDefault()) {
      viaDef->setDefault(true);
    }
    for (auto box : via->getBoxes()) {
      frLayerNum layerNum;
      std::string layer = box->getTechLayer()->getName();
      if (tech_->name2layer_.find(layer) == tech_->name2layer_.end()) {
        logger_->error(
            DRT, 127, "Unknown layer {} for via {}.", layer, via->getName());
      } else {
        layerNum = tech_->name2layer_.at(layer)->getLayerNum();
      }
      frCoord xl = box->xMin();
      frCoord yl = box->yMin();
      frCoord xh = box->xMax();
      frCoord yh = box->yMax();
      std::unique_ptr<frRect> pinFig = std::make_unique<frRect>();
      pinFig->setBBox(Rect(xl, yl, xh, yh));
      pinFig->setLayerNum(layerNum);
      if (lNum2Int[layerNum] == 1) {
        viaDef->addLayer1Fig(std::move(pinFig));
      } else if (lNum2Int[layerNum] == 3) {
        viaDef->addLayer2Fig(std::move(pinFig));
      } else if (lNum2Int[layerNum] == 2) {
        viaDef->addCutFig(std::move(pinFig));
      }
    }
    auto cutLayerNum = viaDef->getCutLayerNum();
    auto cutLayer = tech_->getLayer(cutLayerNum);
    int cutClassIdx = -1;
    frLef58CutClass* cutClass = nullptr;

    for (auto& cutFig : viaDef->getCutFigs()) {
      Rect box = cutFig->getBBox();
      auto width = box.minDXDY();
      auto length = box.maxDXDY();
      cutClassIdx = cutLayer->getCutClassIdx(width, length);
      if (cutClassIdx != -1) {
        cutClass = cutLayer->getCutClass(cutClassIdx);
        break;
      }
    }
    if (cutClass) {
      viaDef->setCutClass(cutClass);
      viaDef->setCutClassIdx(cutClassIdx);
    }
    if (!tech_->addVia(std::move(viaDef))) {
      logger_->error(
          utl::DRT, 339, "Duplicated via definition for {}", via->getName());
    }
  }
}

void io::Parser::readTechAndLibs(odb::dbDatabase* db)
{
  if (VERBOSE > 0) {
    logger_->info(DRT, 149, "Reading tech and libs.");
  }

  auto tech = db->getTech();
  if (tech == nullptr) {
    logger_->error(DRT, 136, "Load design first.");
  }
  tech_->setDBUPerUU(tech->getDbUnitsPerMicron());
  USEMINSPACING_OBS = tech->getUseMinSpacingObs() == odb::dbOnOffType::ON;
  tech_->setManufacturingGrid(frUInt4(tech->getManufacturingGrid()));
  setLayers(tech);

  auto fr_tech = design_->getTech();
  if (!BOTTOM_ROUTING_LAYER_NAME.empty()) {
    frLayer* layer = fr_tech->getLayer(BOTTOM_ROUTING_LAYER_NAME);
    if (layer) {
      BOTTOM_ROUTING_LAYER = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    272,
                    "bottomRoutingLayer {} not found.",
                    BOTTOM_ROUTING_LAYER_NAME);
    }
  }

  if (!TOP_ROUTING_LAYER_NAME.empty()) {
    frLayer* layer = fr_tech->getLayer(TOP_ROUTING_LAYER_NAME);
    if (layer) {
      TOP_ROUTING_LAYER = layer->getLayerNum();
    } else {
      logger_->warn(utl::DRT,
                    273,
                    "topRoutingLayer {} not found.",
                    TOP_ROUTING_LAYER_NAME);
    }
  }

  setTechVias(db->getTech());
  setTechViaRules(db->getTech());
  if (db->getChip() && db->getChip()->getBlock()) {
    setVias(db->getChip()->getBlock());
  }
  setMasters(db);
  setNDRs(db);
  initDefaultVias();

  if (VERBOSE > 0) {
    logger_->report("");
    logger_->report("Units:                {}", tech_->getDBUPerUU());
    logger_->report("Number of layers:     {}", tech_->layers_.size());
    logger_->report("Number of macros:     {}", design_->masters_.size());
    logger_->report("Number of vias:       {}", tech_->vias_.size());
    logger_->report("Number of viarulegen: {}",
                    tech_->viaRuleGenerates_.size());
    logger_->report("");
  }
}

bool io::Parser::readGuide()
{
  ProfileTask profile("IO:readGuide");
  int numGuides = 0;
  auto block = db_->getChip()->getBlock();
  for (auto dbNet : block->getNets()) {
    if (dbNet->getGuides().empty()) {
      continue;
    }
    frNet* net = design_->topBlock_->findNet(dbNet->getName());
    if (net == nullptr) {
      logger_->error(DRT, 153, "Cannot find net {}.", dbNet->getName());
    }
    for (auto dbGuide : dbNet->getGuides()) {
      frLayer* layer = design_->tech_->getLayer(dbGuide->getLayer()->getName());
      if (layer == nullptr) {
        logger_->error(
            DRT, 154, "Cannot find layer {}.", dbGuide->getLayer()->getName());
      }
      frLayerNum layerNum = layer->getLayerNum();

      // get the top layer for a pin of the net
      bool isAboveTopLayer = false;
      for (const auto& bterm : net->getBTerms()) {
        isAboveTopLayer = bterm->isAboveTopLayer();
      }

      // update the layer of the guides above the top routing layer
      // if the guides are used to access a pin above the top routing layer
      if (layerNum > TOP_ROUTING_LAYER && isAboveTopLayer) {
        continue;
      }
      if ((layerNum < BOTTOM_ROUTING_LAYER && layerNum != VIA_ACCESS_LAYERNUM)
          || layerNum > TOP_ROUTING_LAYER) {
        logger_->error(DRT,
                       155,
                       "Guide in net {} uses layer {} ({})"
                       " that is outside the allowed routing range "
                       "[{} ({}), {} ({})] with via access on [{} ({})].",
                       net->getName(),
                       layer->getName(),
                       layerNum,
                       tech_->getLayer(BOTTOM_ROUTING_LAYER)->getName(),
                       BOTTOM_ROUTING_LAYER,
                       tech_->getLayer(TOP_ROUTING_LAYER)->getName(),
                       TOP_ROUTING_LAYER,
                       tech_->getLayer(VIA_ACCESS_LAYERNUM)->getName(),
                       VIA_ACCESS_LAYERNUM);
      }

      frRect rect;
      rect.setBBox(dbGuide->getBox());
      rect.setLayerNum(layerNum);
      tmpGuides_[net].push_back(rect);
      ++numGuides;
      if (numGuides < 1000000) {
        if (numGuides % 100000 == 0) {
          logger_->info(DRT, 156, "guideIn read {} guides.", numGuides);
        }
      } else {
        if (numGuides % 1000000 == 0) {
          logger_->info(DRT, 157, "guideIn read {} guides.", numGuides);
        }
      }
    }
  }
  if (VERBOSE > 0) {
    logger_->report("");
    logger_->report("Number of guides:     {}", numGuides);
    logger_->report("");
  }
  return !tmpGuides_.empty();
}

void io::Writer::fillConnFigs_net(frNet* net, bool isTA)
{
  const auto& netName = net->getName();
  if (isTA) {
    for (auto& uGuide : net->getGuides()) {
      // std::cout <<"find guide" <<std::endl;
      for (auto& uConnFig : uGuide->getRoutes()) {
        auto connFig = uConnFig.get();
        if (connFig->typeId() == frcPathSeg) {
          connFigs_[netName].push_back(
              std::make_shared<frPathSeg>(*static_cast<frPathSeg*>(connFig)));
        } else if (connFig->typeId() == frcVia) {
          connFigs_[netName].push_back(
              std::make_shared<frVia>(*static_cast<frVia*>(connFig)));
        } else {
          logger_->warn(
              DRT,
              247,
              "io::Writer::fillConnFigs_net does not support this type.");
        }
      }
    }
  } else {
    for (auto& shape : net->getShapes()) {
      if (shape->typeId() == frcPathSeg) {
        auto pathSeg = *static_cast<frPathSeg*>(shape.get());
        connFigs_[netName].push_back(std::make_shared<frPathSeg>(pathSeg));
      }
    }
    for (auto& via : net->getVias()) {
      connFigs_[netName].push_back(std::make_shared<frVia>(*via));
    }
    for (auto& shape : net->getPatchWires()) {
      auto pwire = static_cast<frPatchWire*>(shape.get());
      connFigs_[netName].push_back(std::make_shared<frPatchWire>(*pwire));
    }
  }
}

void io::Writer::splitVia_helper(
    frLayerNum layerNum,
    int isH,
    frCoord trackLoc,
    frCoord x,
    frCoord y,
    std::vector<std::vector<
        std::map<frCoord, std::vector<std::shared_ptr<frPathSeg>>>>>&
        mergedPathSegs)
{
  if (layerNum >= 0 && layerNum < (int) (getTech()->getLayers().size())
      && mergedPathSegs.at(layerNum).at(isH).find(trackLoc)
             != mergedPathSegs.at(layerNum).at(isH).end()) {
    for (auto& pathSeg : mergedPathSegs.at(layerNum).at(isH).at(trackLoc)) {
      auto [begin, end] = pathSeg->getPoints();
      if ((isH == 0 && (begin.x() < x) && (end.x() > x))
          || (isH == 1 && (begin.y() < y) && (end.y() > y))) {
        frSegStyle style1 = pathSeg->getStyle();
        frSegStyle style2 = pathSeg->getStyle();
        frSegStyle style_default
            = getTech()->getLayer(layerNum)->getDefaultSegStyle();
        std::shared_ptr<frPathSeg> newPathSeg
            = std::make_shared<frPathSeg>(*pathSeg);
        pathSeg->setPoints(begin, Point(x, y));
        style1.setEndStyle(style_default.getEndStyle(),
                           style_default.getEndExt());
        pathSeg->setStyle(style1);
        newPathSeg->setPoints(Point(x, y), end);
        style2.setBeginStyle(style_default.getBeginStyle(),
                             style_default.getBeginExt());
        newPathSeg->setStyle(style2);
        mergedPathSegs.at(layerNum).at(isH).at(trackLoc).push_back(newPathSeg);
        // via can only intersect at most one merged pathseg on one track
        break;
      }
    }
  }
}

// merge pathseg, delete redundant via
void io::Writer::mergeSplitConnFigs(
    std::list<std::shared_ptr<frConnFig>>& connFigs)
{
  // if (VERBOSE > 0) {
  //   std::cout <<std::endl << "merge and split." <<std::endl;
  // }
  //  initialize pathseg and via map
  std::map<std::tuple<frLayerNum, bool, frCoord>,
           std::map<frCoord,
                    std::vector<std::tuple<std::shared_ptr<frPathSeg>, bool>>>>
      pathSegMergeMap;
  std::map<std::tuple<frCoord, frCoord, frLayerNum>, std::shared_ptr<frVia>>
      viaMergeMap;
  for (auto& connFig : connFigs) {
    if (connFig->typeId() == frcPathSeg) {
      auto pathSeg = std::dynamic_pointer_cast<frPathSeg>(connFig);
      auto [begin, end] = pathSeg->getPoints();
      frLayerNum layerNum = pathSeg->getLayerNum();
      if (begin == end) {
        // std::cout << "Warning: 0 length connFig\n";
        continue;  // if segment length = 0, ignore
      }
      // std::cout << "xxx\n";
      bool isH = (begin.x() == end.x()) ? false : true;
      frCoord trackLoc = isH ? begin.y() : begin.x();
      frCoord beginCoord = isH ? begin.x() : begin.y();
      frCoord endCoord = isH ? end.x() : end.y();
      pathSegMergeMap[std::make_tuple(layerNum, isH, trackLoc)][beginCoord]
          .push_back(std::make_tuple(pathSeg, true));
      pathSegMergeMap[std::make_tuple(layerNum, isH, trackLoc)][endCoord]
          .push_back(std::make_tuple(pathSeg, false));

    } else if (connFig->typeId() == frcVia) {
      auto via = std::dynamic_pointer_cast<frVia>(connFig);
      auto cutLayerNum = via->getViaDef()->getCutLayerNum();
      Point viaPoint = via->getOrigin();
      viaMergeMap[std::make_tuple(viaPoint.x(), viaPoint.y(), cutLayerNum)]
          = via;
      // std::cout <<"found via" <<std::endl;
    }
  }

  // merge pathSeg
  std::map<frCoord, std::vector<std::shared_ptr<frPathSeg>>> tmp1;
  std::vector<std::map<frCoord, std::vector<std::shared_ptr<frPathSeg>>>> tmp2(
      2, tmp1);
  std::vector<
      std::vector<std::map<frCoord, std::vector<std::shared_ptr<frPathSeg>>>>>
      mergedPathSegs(getTech()->getLayers().size(), tmp2);

  for (auto& it1 : pathSegMergeMap) {
    auto layerNum = std::get<0>(it1.first);
    int isH = std::get<1>(it1.first);
    auto trackLoc = std::get<2>(it1.first);
    bool hasSeg = false;
    int cnt = 0;
    std::shared_ptr<frPathSeg> newPathSeg;
    frSegStyle style;
    for (auto& it2 : it1.second) {
      // std::cout <<"coord " <<coord <<std::endl;
      for (auto& pathSegTuple : it2.second) {
        cnt += std::get<1>(pathSegTuple) ? 1 : -1;
      }
      // newPathSeg begin
      if (!hasSeg && cnt > 0) {
        style.setBeginStyle(frcTruncateEndStyle, 0);
        style.setEndStyle(frcTruncateEndStyle, 0);
        newPathSeg = std::make_shared<frPathSeg>(
            *(std::get<0>(*(it2.second.begin()))));
        for (auto& pathSegTuple : it2.second) {
          auto pathSeg = std::get<0>(pathSegTuple);
          auto isBegin = std::get<1>(pathSegTuple);
          if (isBegin) {
            frSegStyle tmpStyle = pathSeg->getStyle();
            if (tmpStyle.getBeginExt() > style.getBeginExt()) {
              style.setBeginStyle(tmpStyle.getBeginStyle(),
                                  tmpStyle.getBeginExt());
            }
          }
        }
        newPathSeg->setStyle(style);
        hasSeg = true;
        // newPathSeg end
      } else if (hasSeg && cnt == 0) {
        auto [begin, end] = newPathSeg->getPoints();
        for (auto& pathSegTuple : it2.second) {
          auto pathSeg = std::get<0>(pathSegTuple);
          auto isBegin = std::get<1>(pathSegTuple);
          if (!isBegin) {
            Point tmp;
            std::tie(tmp, end) = pathSeg->getPoints();
            frSegStyle tmpStyle = pathSeg->getStyle();
            if (tmpStyle.getEndExt() > style.getEndExt()) {
              style.setEndStyle(tmpStyle.getEndStyle(), tmpStyle.getEndExt());
            }
          }
        }
        newPathSeg->setPoints(begin, end);
        newPathSeg->setStyle(style);
        hasSeg = false;
        (mergedPathSegs.at(layerNum).at(isH))[trackLoc].push_back(newPathSeg);
      }
    }
  }

  // split pathseg from via
  // mergedPathSegs[layerNum][isHorizontal] is a std::map<frCoord,
  // std::vector<std::shared_ptr<frPathSeg> > >
  // map < std::tuple<frCoord, frCoord, frLayerNum>, std::shared_ptr<frVia> >
  // viaMergeMap;
  for (auto& it1 : viaMergeMap) {
    auto x = std::get<0>(it1.first);
    auto y = std::get<1>(it1.first);
    auto cutLayerNum = std::get<2>(it1.first);
    frCoord trackLoc;

    auto layerNum = cutLayerNum - 1;
    int isH = 1;
    trackLoc = (isH == 1) ? y : x;
    splitVia_helper(layerNum, isH, trackLoc, x, y, mergedPathSegs);

    layerNum = cutLayerNum - 1;
    isH = 0;
    trackLoc = (isH == 1) ? y : x;
    splitVia_helper(layerNum, isH, trackLoc, x, y, mergedPathSegs);

    layerNum = cutLayerNum + 1;
    trackLoc = (isH == 1) ? y : x;
    splitVia_helper(layerNum, isH, trackLoc, x, y, mergedPathSegs);

    layerNum = cutLayerNum + 1;
    isH = 0;
    trackLoc = (isH == 1) ? y : x;
    splitVia_helper(layerNum, isH, trackLoc, x, y, mergedPathSegs);
  }

  // split intersecting pathSegs
  for (auto& it1 : mergedPathSegs) {
    // vertical for mapIt1
    for (auto& mapIt1 : it1.at(0)) {
      // horizontal for mapIt2
      for (auto& mapIt2 : it1.at(1)) {
        // at most split once
        // seg1 is vertical
        for (auto& seg1 : mapIt1.second) {
          bool skip = false;
          // seg2 is horizontal
          auto [seg1Begin, seg1End] = seg1->getPoints();
          for (auto& seg2 : mapIt2.second) {
            auto [seg2Begin, seg2End] = seg2->getPoints();
            bool pushNewSeg1 = false;
            bool pushNewSeg2 = false;
            std::shared_ptr<frPathSeg> newSeg1;
            std::shared_ptr<frPathSeg> newSeg2;
            // check whether seg1 needs to be split, break seg1
            if (seg2Begin.y() > seg1Begin.y() && seg2Begin.y() < seg1End.y()) {
              pushNewSeg1 = true;
              newSeg1 = std::make_shared<frPathSeg>(*seg1);
              // modify seg1
              seg1->setPoints(seg1Begin, Point(seg1End.x(), seg2End.y()));
              // modify newSeg1
              newSeg1->setPoints(Point(seg1End.x(), seg2Begin.y()), seg1End);
              // modify endstyle
              auto layerNum = seg1->getLayerNum();
              frSegStyle tmpStyle1 = seg1->getStyle();
              frSegStyle tmpStyle2 = seg1->getStyle();
              frSegStyle style_default
                  = getTech()->getLayer(layerNum)->getDefaultSegStyle();
              tmpStyle1.setEndStyle(frcExtendEndStyle,
                                    style_default.getEndExt());
              seg1->setStyle(tmpStyle1);
              tmpStyle2.setBeginStyle(frcExtendEndStyle,
                                      style_default.getBeginExt());
              newSeg1->setStyle(tmpStyle2);
            }
            // check whether seg2 needs to be split, break seg2
            if (seg1Begin.x() > seg2Begin.x() && seg1Begin.x() < seg2End.x()) {
              pushNewSeg2 = true;
              newSeg2 = std::make_shared<frPathSeg>(*seg1);
              // modify seg2
              seg2->setPoints(seg2Begin, Point(seg1End.x(), seg2End.y()));
              // modify newSeg2
              newSeg2->setPoints(Point(seg1End.x(), seg2Begin.y()), seg2End);
              // modify endstyle
              auto layerNum = seg2->getLayerNum();
              frSegStyle tmpStyle1 = seg2->getStyle();
              frSegStyle tmpStyle2 = seg2->getStyle();
              frSegStyle style_default
                  = getTech()->getLayer(layerNum)->getDefaultSegStyle();
              tmpStyle1.setEndStyle(frcExtendEndStyle,
                                    style_default.getEndExt());
              seg2->setStyle(tmpStyle1);
              tmpStyle2.setBeginStyle(frcExtendEndStyle,
                                      style_default.getBeginExt());
              newSeg2->setStyle(tmpStyle2);
            }
            if (pushNewSeg1) {
              mapIt1.second.push_back(newSeg1);
            }
            if (pushNewSeg2) {
              mapIt2.second.push_back(newSeg2);
            }
            if (pushNewSeg1 || pushNewSeg2) {
              skip = true;
              break;
            }
            // std::cout <<"found" <<std::endl;
          }
          if (skip) {
            break;
          }
        }
      }
    }
  }

  // write back pathseg
  connFigs.clear();
  for (auto& it1 : mergedPathSegs) {
    for (auto& it2 : it1) {
      for (auto& it3 : it2) {
        for (auto& it4 : it3.second) {
          connFigs.push_back(it4);
        }
      }
    }
  }

  // write back via
  // map < std::tuple<frCoord, frCoord, frLayerNum>, std::shared_ptr<frVia> >
  // viaMergeMap;
  for (auto& it : viaMergeMap) {
    connFigs.push_back(it.second);
  }
}

void io::Writer::fillViaDefs()
{
  viaDefs_.clear();
  for (auto& uViaDef : getDesign()->getTech()->getVias()) {
    auto viaDef = uViaDef.get();
    if (viaDef->isAddedByRouter()) {
      viaDefs_.push_back(viaDef);
    }
  }
}

void io::Writer::fillConnFigs(bool isTA)
{
  connFigs_.clear();
  if (VERBOSE > 0) {
    logger_->info(DRT, 180, "Post processing.");
  }
  for (auto& net : getDesign()->getTopBlock()->getNets()) {
    fillConnFigs_net(net.get(), isTA);
  }
  if (isTA) {
    for (auto& it : connFigs_) {
      mergeSplitConnFigs(it.second);
    }
  }
}

void io::Writer::updateDbVias(odb::dbBlock* block, odb::dbTech* db_tech)
{
  for (auto via : viaDefs_) {
    if (block->findVia(via->getName().c_str()) != nullptr) {
      continue;
    }
    auto layer1Name = getTech()->getLayer(via->getLayer1Num())->getName();
    auto layer2Name = getTech()->getLayer(via->getLayer2Num())->getName();
    auto cutName = getTech()->getLayer(via->getCutLayerNum())->getName();
    odb::dbTechLayer* _layer1 = db_tech->findLayer(layer1Name.c_str());
    odb::dbTechLayer* _layer2 = db_tech->findLayer(layer2Name.c_str());
    odb::dbTechLayer* _cut_layer = db_tech->findLayer(cutName.c_str());
    if (_layer1 == nullptr || _layer2 == nullptr || _cut_layer == nullptr) {
      logger_->error(DRT,
                     113,
                     "Tech layers for via {} not found in db tech.",
                     via->getName());
    }
    odb::dbVia* _db_via = odb::dbVia::create(block, via->getName().c_str());
    _db_via->setDefault(true);
    for (auto& fig : via->getLayer2Figs()) {
      Rect box = fig->getBBox();
      odb::dbBox::create(
          _db_via, _layer2, box.xMin(), box.yMin(), box.xMax(), box.yMax());
    }
    for (auto& fig : via->getCutFigs()) {
      Rect box = fig->getBBox();
      odb::dbBox::create(
          _db_via, _cut_layer, box.xMin(), box.yMin(), box.xMax(), box.yMax());
    }

    for (auto& fig : via->getLayer1Figs()) {
      Rect box = fig->getBBox();
      odb::dbBox::create(
          _db_via, _layer1, box.xMin(), box.yMin(), box.xMax(), box.yMax());
    }
  }
}

void io::Writer::updateDbConn(odb::dbBlock* block,
                              odb::dbTech* db_tech,
                              bool snapshot)
{
  odb::dbWireEncoder _wire_encoder;
  for (auto net : block->getNets()) {
    if (connFigs_.find(net->getName()) != connFigs_.end()) {
      odb::dbWire* wire = net->getWire();
      if (wire == nullptr) {
        wire = odb::dbWire::create(net);
        _wire_encoder.begin(wire);
      } else if (snapshot) {
        odb::dbWire::destroy(wire);
        wire = odb::dbWire::create(net);
        _wire_encoder.begin(wire);
      } else {
        _wire_encoder.append(wire);
      }

      for (auto& connFig : connFigs_.at(net->getName())) {
        switch (connFig->typeId()) {
          case frcPathSeg: {
            auto pathSeg = std::dynamic_pointer_cast<frPathSeg>(connFig);
            auto layerName
                = getTech()->getLayer(pathSeg->getLayerNum())->getName();
            auto layer = db_tech->findLayer(layerName.c_str());
            if (pathSeg->isTapered() || !net->getNonDefaultRule()) {
              _wire_encoder.newPath(layer, odb::dbWireType("ROUTED"));
            } else {
              _wire_encoder.newPath(
                  layer,
                  odb::dbWireType("ROUTED"),
                  net->getNonDefaultRule()->getLayerRule(layer));
            }
            auto [begin, end] = pathSeg->getPoints();
            frSegStyle segStyle = pathSeg->getStyle();
            if (segStyle.getBeginStyle() == frEndStyle(frcExtendEndStyle)) {
              if (segStyle.getBeginExt() != layer->getWidth() / 2) {
                _wire_encoder.addPoint(
                    begin.x(), begin.y(), segStyle.getBeginExt(), 0);
              } else {
                _wire_encoder.addPoint(begin.x(), begin.y());
              }
            } else if (segStyle.getBeginStyle()
                       == frEndStyle(frcTruncateEndStyle)) {
              _wire_encoder.addPoint(begin.x(), begin.y(), 0, 0);
            } else if (segStyle.getBeginStyle()
                       == frEndStyle(frcVariableEndStyle)) {
              _wire_encoder.addPoint(
                  begin.x(), begin.y(), segStyle.getBeginExt(), 0);
            }
            if (segStyle.getEndStyle() == frEndStyle(frcExtendEndStyle)) {
              if (segStyle.getEndExt() != layer->getWidth() / 2) {
                _wire_encoder.addPoint(
                    end.x(), end.y(), segStyle.getEndExt(), 0);
              } else {
                _wire_encoder.addPoint(end.x(), end.y());
              }
            } else if (segStyle.getEndStyle()
                       == frEndStyle(frcTruncateEndStyle)) {
              _wire_encoder.addPoint(end.x(), end.y(), 0, 0);
            } else if (segStyle.getBeginStyle()
                       == frEndStyle(frcVariableEndStyle)) {
              _wire_encoder.addPoint(end.x(), end.y(), segStyle.getEndExt(), 0);
            }
            break;
          }
          case frcVia: {
            auto via = std::dynamic_pointer_cast<frVia>(connFig);
            auto layerName = getTech()
                                 ->getLayer(via->getViaDef()->getLayer1Num())
                                 ->getName();
            auto viaName = via->getViaDef()->getName();
            auto layer = db_tech->findLayer(layerName.c_str());
            if (!net->getNonDefaultRule() || via->isTapered()) {
              _wire_encoder.newPath(layer, odb::dbWireType("ROUTED"));
            } else {
              _wire_encoder.newPath(
                  layer,
                  odb::dbWireType("ROUTED"),
                  net->getNonDefaultRule()->getLayerRule(layer));
            }
            Point origin = via->getOrigin();
            _wire_encoder.addPoint(origin.x(), origin.y());
            odb::dbTechVia* tech_via = db_tech->findVia(viaName.c_str());
            if (tech_via != nullptr) {
              _wire_encoder.addTechVia(tech_via);
            } else {
              odb::dbVia* db_via = block->findVia(viaName.c_str());
              _wire_encoder.addVia(db_via);
            }
            break;
          }
          case frcPatchWire: {
            auto pwire = std::dynamic_pointer_cast<frPatchWire>(connFig);
            auto layerName
                = getTech()->getLayer(pwire->getLayerNum())->getName();
            auto layer = db_tech->findLayer(layerName.c_str());
            _wire_encoder.newPath(layer, odb::dbWireType("ROUTED"));
            Point origin = pwire->getOrigin();
            Rect offsetBox = pwire->getOffsetBox();
            _wire_encoder.addPoint(origin.x(), origin.y());
            _wire_encoder.addRect(offsetBox.xMin(),
                                  offsetBox.yMin(),
                                  offsetBox.xMax(),
                                  offsetBox.yMax());
            break;
          }
          default: {
            _wire_encoder.clear();
            logger_->error(DRT,
                           114,
                           "Unknown connFig type while writing net {}.",
                           net->getName());
          }
        }
      }
      _wire_encoder.end();
      net->setWireOrdered(false);
    }
  }
}

void updateDbAccessPoint(odb::dbAccessPoint* db_ap,
                         frAccessPoint* ap,
                         odb::dbTech* db_tech,
                         frTechObject* tech,
                         odb::dbBlock* block)
{
  db_ap->setPoint(ap->getPoint());
  if (ap->hasAccess(frDirEnum::N)) {
    db_ap->setAccess(true, odb::dbDirection::NORTH);
  }
  if (ap->hasAccess(frDirEnum::S)) {
    db_ap->setAccess(true, odb::dbDirection::SOUTH);
  }
  if (ap->hasAccess(frDirEnum::E)) {
    db_ap->setAccess(true, odb::dbDirection::EAST);
  }
  if (ap->hasAccess(frDirEnum::W)) {
    db_ap->setAccess(true, odb::dbDirection::WEST);
  }
  if (ap->hasAccess(frDirEnum::U)) {
    db_ap->setAccess(true, odb::dbDirection::UP);
  }
  if (ap->hasAccess(frDirEnum::D)) {
    db_ap->setAccess(true, odb::dbDirection::DOWN);
  }
  auto layer = db_tech->findLayer(
      tech->getLayer(ap->getLayerNum())->getName().c_str());
  db_ap->setLayer(layer);
  db_ap->setLowType((odb::dbAccessType::Value) ap->getType(
      true));  // this works because both enums have the same order
  db_ap->setHighType((odb::dbAccessType::Value) ap->getType(false));
  auto allViaDefs = ap->getAllViaDefs();
  int numCuts = 1;
  for (const auto& cutViaDefs : allViaDefs) {
    for (const auto& viaDef : cutViaDefs) {
      if (db_tech->findVia(viaDef->getName().c_str()) != nullptr) {
        db_ap->addTechVia(numCuts, db_tech->findVia(viaDef->getName().c_str()));
      } else {
        db_ap->addBlockVia(numCuts, block->findVia(viaDef->getName().c_str()));
      }
    }
    ++numCuts;
  }
  auto path_segs = ap->getPathSegs();
  for (const auto& path_seg : path_segs) {
    Rect db_rect = Rect(path_seg.getBeginPoint(), path_seg.getEndPoint());
    bool begin_style_trunc = (path_seg.getBeginStyle() == frcTruncateEndStyle);
    bool end_style_trunc = (path_seg.getEndStyle() == frcTruncateEndStyle);
    db_ap->addSegment(db_rect, begin_style_trunc, end_style_trunc);
  }
}

void io::Writer::updateTrackAssignment(odb::dbBlock* block)
{
  for (const auto& net : design_->getTopBlock()->getNets()) {
    auto dbNet = block->findNet(net->getName().c_str());
    for (const auto& guide : net->getGuides()) {
      for (const auto& route : guide->getRoutes()) {
        frPathSeg* track = static_cast<frPathSeg*>(route.get());
        auto layer = design_->getTech()->getLayer(track->getLayerNum());
        odb::dbNetTrack::create(dbNet, layer->getDbLayer(), track->getBBox());
      }
    }
  }
}

void io::Writer::updateDbAccessPoints(odb::dbBlock* block, odb::dbTech* db_tech)
{
  for (auto ap : block->getAccessPoints()) {
    odb::dbAccessPoint::destroy(ap);
  }
  auto db = block->getDb();
  std::map<frAccessPoint*, odb::dbAccessPoint*> aps_map;
  for (auto& master : design_->getMasters()) {
    auto db_master = db->findMaster(master->getName().c_str());
    if (db_master == nullptr) {
      logger_->error(DRT, 294, "master {} not found in db", master->getName());
    }
    for (auto& term : master->getTerms()) {
      auto db_mterm = db_master->findMTerm(term->getName().c_str());
      if (db_mterm == nullptr) {
        logger_->error(DRT, 295, "mterm {} not found in db", term->getName());
      }
      auto db_pins = db_mterm->getMPins();
      if (db_pins.size() != term->getPins().size()) {
        logger_->error(DRT,
                       296,
                       "Mismatch in number of pins for term {}/{}",
                       master->getName(),
                       term->getName());
      }
      frUInt4 i = 0;
      auto& pins = term->getPins();
      for (auto db_pin : db_pins) {
        auto& pin = pins[i++];
        int j = 0;
        int sz = pin->getNumPinAccess();
        while (j < sz) {
          auto pa = pin->getPinAccess(j);
          for (auto& ap : pa->getAccessPoints()) {
            auto db_ap = odb::dbAccessPoint::create(block, db_pin, j);
            updateDbAccessPoint(db_ap, ap.get(), db_tech, getTech(), block);
            aps_map[ap.get()] = db_ap;
          }
          j++;
        }
      }
    }
  }
  for (auto& inst : design_->getTopBlock()->getInsts()) {
    auto db_inst = block->findInst(inst->getName().c_str());
    if (db_inst == nullptr) {
      logger_->error(DRT, 297, "inst {} not found in db", inst->getName());
    }
    db_inst->setPinAccessIdx(inst->getPinAccessIdx());
    for (auto& term : inst->getInstTerms()) {
      auto aps = term->getAccessPoints();
      auto db_iterm = db_inst->findITerm(term->getTerm()->getName().c_str());
      if (db_iterm == nullptr) {
        logger_->error(DRT, 298, "iterm {} not found in db", term->getName());
      }
      auto db_pins = db_iterm->getMTerm()->getMPins();
      if (aps.size() != db_pins.size()) {
        logger_->error(
            DRT,
            299,
            "Mismatch in access points size {} and term pins size {}",
            aps.size(),
            db_pins.size());
      }
      frUInt4 i = 0;
      for (auto db_pin : db_pins) {
        if (aps[i] != nullptr) {
          if (aps_map.find(aps[i]) != aps_map.end()) {
            db_iterm->setAccessPoint(db_pin, aps_map[aps[i]]);
          } else {
            logger_->error(DRT, 300, "Preferred access point is not found");
          }
        } else {
          db_iterm->setAccessPoint(db_pin, nullptr);
        }
        i++;
      }
    }
  }
  for (auto& term : design_->getTopBlock()->getTerms()) {
    auto db_term = block->findBTerm(term->getName().c_str());
    if (db_term == nullptr) {
      logger_->error(DRT, 301, "bterm {} not found in db", term->getName());
    }
    auto db_net = db_term->getNet();
    if (db_term->getSigType().isSupply() || (db_net && db_net->isSpecial())) {
      continue;
    }
    auto db_pins = db_term->getBPins();
    auto& pins = term->getPins();
    if (db_pins.size() != pins.size()) {
      logger_->error(
          DRT, 303, "Mismatch in number of pins for bterm {}", term->getName());
    }
    if (pins.size() != 1) {
      continue;
    }
    auto db_pin = (odb::dbBPin*) *db_pins.begin();
    auto& pin = pins[0];
    int j = 0;
    int sz = pin->getNumPinAccess();
    while (j < sz) {
      auto pa = pin->getPinAccess(j);
      for (auto& ap : pa->getAccessPoints()) {
        auto db_ap = odb::dbAccessPoint::create(db_pin);
        updateDbAccessPoint(db_ap, ap.get(), db_tech, getTech(), block);
      }
      j++;
    }
  }
}

void io::Writer::updateDb(odb::dbDatabase* db, bool pin_access, bool snapshot)
{
  if (db->getChip() == nullptr) {
    logger_->error(DRT, 3, "Load design first.");
  }

  odb::dbBlock* block = db->getChip()->getBlock();
  odb::dbTech* db_tech = db->getTech();
  if (block == nullptr || db_tech == nullptr) {
    logger_->error(DRT, 4, "Load design first.");
  }
  fillViaDefs();
  updateDbVias(block, db_tech);
  updateDbAccessPoints(block, db_tech);
  if (!pin_access) {
    fillConnFigs(false);
    updateDbConn(block, db_tech, snapshot);
  }
}

}  // namespace drt
