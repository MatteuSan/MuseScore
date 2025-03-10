/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "systemlayout.h"

#include "realfn.h"

#include "style/defaultstyle.h"

#include "libmscore/barline.h"
#include "libmscore/beam.h"
#include "libmscore/box.h"
#include "libmscore/bracket.h"
#include "libmscore/bracketItem.h"
#include "libmscore/chord.h"
#include "libmscore/dynamic.h"
#include "libmscore/factory.h"
#include "libmscore/instrumentname.h"
#include "libmscore/layoutbreak.h"
#include "libmscore/measure.h"
#include "libmscore/measurenumber.h"
#include "libmscore/mmrestrange.h"
#include "libmscore/note.h"
#include "libmscore/ornament.h"
#include "libmscore/part.h"
#include "libmscore/rest.h"
#include "libmscore/score.h"
#include "libmscore/slur.h"
#include "libmscore/spacer.h"
#include "libmscore/staff.h"
#include "libmscore/stafflines.h"
#include "libmscore/stretchedbend.h"
#include "libmscore/system.h"
#include "libmscore/tie.h"
#include "libmscore/tremolo.h"
#include "libmscore/tuplet.h"
#include "libmscore/volta.h"

#include "tlayout.h"
#include "beamlayout.h"
#include "chordlayout.h"
#include "harmonylayout.h"
#include "lyricslayout.h"
#include "measurelayout.h"
#include "tupletlayout.h"
#include "slurtielayout.h"

#include "log.h"

using namespace mu::engraving;
using namespace mu::engraving::layout::v0;

//---------------------------------------------------------
//   collectSystem
//---------------------------------------------------------

System* SystemLayout::collectSystem(const LayoutOptions& options, LayoutContext& ctx, Score* score)
{
    TRACEFUNC;

    if (!ctx.curMeasure) {
        return nullptr;
    }

    const MeasureBase* measure  = score->systems().empty() ? 0 : score->systems().back()->measures().back();
    if (measure) {
        measure = measure->findPotentialSectionBreak();
    }

    if (measure) {
        const LayoutBreak* layoutBreak = measure->sectionBreakElement();
        ctx.firstSystem        = measure->sectionBreak() && !options.isMode(LayoutMode::FLOAT);
        ctx.firstSystemIndent  = ctx.firstSystem && options.firstSystemIndent && layoutBreak->firstSystemIndentation();
        ctx.startWithLongNames = ctx.firstSystem && layoutBreak->startWithLongNames();
    }

    System* system = getNextSystem(ctx);
    Fraction lcmTick = ctx.curMeasure->tick();
    SystemLayout::setInstrumentNames(system, ctx, ctx.startWithLongNames, lcmTick);

    double curSysWidth = 0.0;
    double layoutSystemMinWidth = 0.0;
    bool firstMeasure = true;
    bool createHeader = false;
    double targetSystemWidth = score->styleD(Sid::pagePrintableWidth) * DPI;
    system->setWidth(targetSystemWidth);

    // save state of measure
    bool curHeader = ctx.curMeasure->header();
    bool curTrailer = ctx.curMeasure->trailer();
    MeasureBase* breakMeasure = nullptr;

    Fraction minTicks = Fraction::max(); // Initializing at highest possible value
    Fraction prevMinTicks = Fraction(1, 1);
    bool minSysTicksChanged = false;
    Fraction maxTicks = Fraction(0, 1); // Initializing at lowest possible value
    Fraction prevMaxTicks = Fraction(1, 1);
    bool maxSysTicksChanged = false;
    static constexpr double squeezability = 0.3; // We may consider exposing in Style settings (M.S.)
    double oldStretch = 1.0;
    double oldWidth = 0.0;
    System* oldSystem = nullptr;

    while (ctx.curMeasure) {      // collect measure for system
        oldSystem = ctx.curMeasure->system();
        system->appendMeasure(ctx.curMeasure);
        if (system->hasCrossStaffOrModifiedBeams()) {
            updateCrossBeams(system, ctx);
        }
        double ww  = 0.0; // width of current measure
        if (ctx.curMeasure->isMeasure()) {
            Measure* m = toMeasure(ctx.curMeasure);
            if (!(oldSystem && oldSystem->page() && oldSystem->page() != ctx.page)) {
                // Construct information that is needed before horizontal spacing
                // (unless the curMeasure we've just collected comes from the next page)
                MeasureLayout::computePreSpacingItems(m, ctx);
            }
            // After appending a new measure, the shortest note in the system may change, in which case
            // we need to recompute the layout of the previous measures. When updating the width of these
            // measures, curSysWidth must be updated accordingly.
            Fraction curMinTicks = m->shortestChordRest();
            Fraction curMaxTicks = m->maxTicks();
            if (curMinTicks < minTicks) {
                prevMinTicks = minTicks; // We save the previous value in case we need to restore it (see later)
                minTicks = curMinTicks;
                minSysTicksChanged = true;
            } else {
                minSysTicksChanged = false;
            }
            if (curMaxTicks > maxTicks) {
                prevMaxTicks = maxTicks;
                maxTicks = curMaxTicks;
                maxSysTicksChanged = true;
            } else {
                maxSysTicksChanged = false;
            }
            if (minSysTicksChanged || maxSysTicksChanged) {
                for (MeasureBase* mb : system->measures()) {
                    if (mb == m) {
                        break; // Cause I want to change only previous measures, not current one
                    }
                    if (mb->isMeasure()) {
                        Measure* mm = toMeasure(mb);
                        double prevWidth = mm->width();
                        MeasureLayout::computeWidth(mm, ctx, minTicks, maxTicks, 1);
                        double newWidth = mm->width();
                        curSysWidth += newWidth - prevWidth;
                    }
                }
            }

            if (firstMeasure) {
                layoutSystemMinWidth = curSysWidth;
                SystemLayout::layoutSystem(system, ctx, curSysWidth, ctx.firstSystem, ctx.firstSystemIndent);
                if (system->hasCrossStaffOrModifiedBeams()) {
                    updateCrossBeams(system, ctx);
                }
                curSysWidth += system->leftMargin();
                if (m->repeatStart()) {
                    Segment* s = m->findSegmentR(SegmentType::StartRepeatBarLine, Fraction(0, 1));
                    if (!s->enabled()) {
                        s->setEnabled(true);
                    }
                }
                MeasureLayout::addSystemHeader(m, ctx.firstSystem, ctx);
                firstMeasure = false;
                createHeader = false;
            } else {
                if (createHeader) {
                    MeasureLayout::addSystemHeader(m, false, ctx);
                    createHeader = false;
                } else if (m->header()) {
                    MeasureLayout::removeSystemHeader(m);
                }
            }

            MeasureLayout::createEndBarLines(m, true, ctx);
            // measures with nobreak cannot end a system
            // thus they will not contain a trailer
            if (m->noBreak()) {
                MeasureLayout::removeSystemTrailer(m, ctx);
            } else {
                MeasureLayout::addSystemTrailer(m, m->nextMeasure(), ctx);
            }
            MeasureLayout::computeWidth(m, ctx, minTicks, maxTicks, 1);
            ww = m->width();
        } else if (ctx.curMeasure->isHBox()) {
            ctx.curMeasure->computeMinWidth();
            ww = ctx.curMeasure->width();
            createHeader = toHBox(ctx.curMeasure)->createSystemHeader();
        } else {
            // vbox:
            MeasureLayout::getNextMeasure(options, ctx);
            SystemLayout::layout2(system, ctx);         // compute staff distances
            return system;
        }

        // check if lc.curMeasure fits, remove if not
        // collect at least one measure and the break
        double acceptanceRange = squeezability * system->squeezableSpace();
        bool doBreak = (system->measures().size() > 1) && ((curSysWidth + ww) > targetSystemWidth + acceptanceRange)
                       && !ctx.prevMeasure->noBreak();
        /* acceptanceRange allows some systems to be initially slightly larger than the margins and be
         * justified by squeezing instead of stretching. Allows to make much better choices of how many
         * measures to fit per system. */
        if (doBreak) {
            breakMeasure = ctx.curMeasure;
            system->removeLastMeasure();
            ctx.curMeasure->setParent(oldSystem);
            while (ctx.prevMeasure && ctx.prevMeasure->noBreak() && system->measures().size() > 1) {
                // remove however many measures are grouped with nobreak, working backwards
                // but if too many are grouped, stop before we get 0 measures left on system
                // TODO: intelligently break group into smaller groups instead
                ctx.tick -= ctx.curMeasure->ticks();
                ctx.measureNo = ctx.curMeasure->no();

                ctx.nextMeasure = ctx.curMeasure;
                ctx.curMeasure  = ctx.prevMeasure;
                ctx.prevMeasure = ctx.curMeasure->prev();

                curSysWidth -= system->lastMeasure()->width();
                system->removeLastMeasure();
                ctx.curMeasure->setParent(oldSystem);
            }
            // If the last appended measure caused a re-layout of the previous measures, now that we are
            // removing it we need to re-layout the previous measures again.
            if (minSysTicksChanged) {
                minTicks = prevMinTicks; // If the last measure caused it to change, now we need to restore it!
            }
            if (maxSysTicksChanged) {
                maxTicks = prevMaxTicks;
            }
            if (minSysTicksChanged || maxSysTicksChanged) {
                for (MeasureBase* mb : system->measures()) {
                    if (mb->isMeasure()) {
                        double prevWidth = toMeasure(mb)->width();
                        MeasureLayout::computeWidth(toMeasure(mb), ctx, minTicks, maxTicks, 1);
                        double newWidth = toMeasure(mb)->width();
                        curSysWidth += newWidth - prevWidth;
                    }
                }
            }
            break;
        }

        if (ctx.prevMeasure && ctx.prevMeasure->isMeasure() && ctx.prevMeasure->system() == system) {
            //
            // now we know that the previous measure is not the last
            // measure in the system and we finally can create the end barline for it

            Measure* m = toMeasure(ctx.prevMeasure);
            // TODO: if lc.curMeasure is a frame, removing the trailer may be premature
            // but merely skipping this code isn't good enough,
            // we need to find the right time to re-enable the trailer,
            // since it seems to be disabled somewhere else
            if (m->trailer()) {
                double ow = m->width();
                MeasureLayout::removeSystemTrailer(m, ctx);
                curSysWidth += m->width() - ow;
            }
            // if the prev measure is an end repeat and the cur measure
            // is an repeat, the createEndBarLines() created an start-end repeat barline
            // and we can remove the start repeat barline of the current barline

            if (ctx.curMeasure->isMeasure()) {
                Measure* m1 = toMeasure(ctx.curMeasure);
                if (m1->repeatStart()) {
                    Segment* s = m1->findSegmentR(SegmentType::StartRepeatBarLine, Fraction(0, 1));
                    if (!s->enabled()) {
                        s->setEnabled(true);
                        MeasureLayout::computeWidth(m1, ctx, minTicks, maxTicks, 1);
                        ww = m1->width();
                    }
                }
            }
            // TODO: we actually still don't know for sure
            // if this will be the last true measure of the system or not
            // since the lc.curMeasure may be a frame
            // but at this point we have no choice but to assume it isn't
            // since we don't know yet if another true measure will fit
            // worst that happens is we don't get the automatic double bar before a courtesy key signature
            curSysWidth += MeasureLayout::createEndBarLines(m, false, ctx);          // create final barLine
        }

        MeasureBase* mb = ctx.curMeasure;
        bool lineBreak  = false;
        switch (options.mode) {
        case LayoutMode::PAGE:
        case LayoutMode::SYSTEM:
            lineBreak = mb->pageBreak() || mb->lineBreak() || mb->sectionBreak();
            break;
        case LayoutMode::FLOAT:
        case LayoutMode::LINE:
        case LayoutMode::HORIZONTAL_FIXED:
            lineBreak = false;
            break;
        }

        // preserve state of next measure (which is about to become current measure)
        if (ctx.nextMeasure) {
            MeasureBase* nmb = ctx.nextMeasure;
            if (nmb->isMeasure() && score->styleB(Sid::createMultiMeasureRests)) {
                Measure* nm = toMeasure(nmb);
                if (nm->hasMMRest()) {
                    nmb = nm->mmRest();
                }
            }
            if (nmb->isMeasure()) {
                oldStretch = toMeasure(nmb)->layoutStretch();
                oldWidth = toMeasure(nmb)->width();
            }
            if (!ctx.curMeasure->noBreak()) {
                // current measure is not a nobreak,
                // so next measure could possibly start a system
                curHeader = nmb->header();
            }
            if (!nmb->noBreak()) {
                // next measure is not a nobreak
                // so it could possibly end a system
                curTrailer = nmb->trailer();
            }
        }

        MeasureLayout::getNextMeasure(options, ctx);

        curSysWidth += ww;

        // ElementType nt = lc.curMeasure ? lc.curMeasure->type() : ElementType::INVALID;
        mb = ctx.curMeasure;
        bool tooWide = false;     // curSysWidth + minMeasureWidth > systemWidth;  // TODO: noBreak
        if (lineBreak || !mb || mb->isVBox() || mb->isTBox() || mb->isFBox() || tooWide) {
            break;
        }
    }

    assert(ctx.prevMeasure);

    if (ctx.endTick < ctx.prevMeasure->tick()) {
        // we've processed the entire range
        // but we need to continue layout until we reach a system whose last measure is the same as previous layout
        if (ctx.prevMeasure == ctx.systemOldMeasure) {
            // this system ends in the same place as the previous layout
            // ok to stop
            if (ctx.curMeasure && ctx.curMeasure->isMeasure()) {
                // we may have previously processed first measure(s) of next system
                // so now we must restore to original state
                Measure* m = toMeasure(ctx.curMeasure);
                if (m->repeatStart()) {
                    Segment* s = m->findSegmentR(SegmentType::StartRepeatBarLine, Fraction(0, 1));
                    if (!s->enabled()) {
                        s->setEnabled(true);
                    }
                }
                const MeasureBase* pbmb = ctx.prevMeasure->findPotentialSectionBreak();
                bool localFirstSystem = pbmb->sectionBreak() && !options.isMode(LayoutMode::FLOAT);
                MeasureBase* nm = breakMeasure ? breakMeasure : m;
                if (curHeader) {
                    MeasureLayout::addSystemHeader(m, localFirstSystem, ctx);
                } else {
                    MeasureLayout::removeSystemHeader(m);
                }
                for (;;) {
                    // TODO: what if the nobreak group takes the entire system - is this correct?
                    if (curTrailer && !m->noBreak()) {
                        MeasureLayout::addSystemTrailer(m, m->nextMeasure(), ctx);
                    } else {
                        MeasureLayout::removeSystemTrailer(m, ctx);
                    }
                    MeasureLayout::computeWidth(m, ctx, m->system()->minSysTicks(), m->system()->maxSysTicks(), oldStretch);
                    m->stretchToTargetWidth(oldWidth);
                    MeasureLayout::layoutMeasureElements(m, ctx);
                    BeamLayout::restoreBeams(m);
                    if (m == nm || !m->noBreak()) {
                        break;
                    }
                    m = m->nextMeasure();
                }
            }
            ctx.rangeDone = true;
        }
    }

    /*************************************************************
     * SYSTEM NOW HAS A COMPLETE SET OF MEASURES
     * Now perform all operation to finalize system.
     * **********************************************************/

    // Brake cross-measure beams
    // Create end barlines
    if (ctx.prevMeasure && ctx.prevMeasure->isMeasure()) {
        Measure* pm = toMeasure(ctx.prevMeasure);
        BeamLayout::breakCrossMeasureBeams(pm, ctx);
        MeasureLayout::createEndBarLines(pm, true, ctx);
    }

    // hide empty staves
    hideEmptyStaves(score, system, ctx.firstSystem);
    // Relayout system to account for newly hidden/unhidden staves
    curSysWidth -= system->leftMargin();
    SystemLayout::layoutSystem(system, ctx, layoutSystemMinWidth, ctx.firstSystem, ctx.firstSystemIndent);
    curSysWidth += system->leftMargin();

    // add system trailer if needed (cautionary time/key signatures etc)
    Measure* lm  = system->lastMeasure();
    if (lm) {
        Measure* nm = lm->nextMeasure();
        if (nm) {
            MeasureLayout::addSystemTrailer(lm, nm, ctx);
        }
    }

    // Recompute measure widths to account for the last changes (barlines, hidden staves, etc)
    // If system is currently larger than margin (because of acceptanceRange) compute width
    // with a reduced pre-stretch, because justifySystem expects curSysWidth < targetWidth
    double preStretch = targetSystemWidth > curSysWidth ? 1.0 : 1 - squeezability;
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* m = toMeasure(mb);
        double oldWidth = m->width();
        MeasureLayout::computeWidth(m, ctx, minTicks, maxTicks, preStretch);
        curSysWidth += m->width() - oldWidth;
    }

    if (curSysWidth > targetSystemWidth) {
        manageNarrowSpacing(system, ctx, curSysWidth, targetSystemWidth, minTicks, maxTicks);
    }

    // JUSTIFY SYSTEM
    // Do not justify last system of a section if curSysWidth is < lastSystemFillLimit
    if (!((ctx.curMeasure == 0 || (lm && lm->sectionBreak()))
          && ((curSysWidth / targetSystemWidth) < score->styleD(Sid::lastSystemFillLimit)))
        && !MScore::noHorizontalStretch) { // debug feature
        justifySystem(system, curSysWidth, targetSystemWidth);
    }

    // LAYOUT MEASURES
    PointF pos;
    firstMeasure = true;
    bool createBrackets = false;
    for (MeasureBase* mb : system->measures()) {
        double ww = mb->width();
        if (mb->isMeasure()) {
            if (firstMeasure) {
                pos.rx() += system->leftMargin();
                firstMeasure = false;
            }
            mb->setPos(pos);
            mb->setParent(system);
            Measure* m = toMeasure(mb);
            MeasureLayout::layoutMeasureElements(m, ctx);
            MeasureLayout::layoutStaffLines(m, ctx);
            if (createBrackets) {
                SystemLayout::addBrackets(system, toMeasure(mb), ctx);
                createBrackets = false;
            }
        } else if (mb->isHBox()) {
            mb->setPos(pos + PointF(toHBox(mb)->topGap(), 0.0));
            TLayout::layout(mb, ctx);
            createBrackets = toHBox(mb)->createSystemHeader();
        } else if (mb->isVBox()) {
            mb->setPos(pos);
        }
        pos.rx() += ww;
    }
    system->setWidth(pos.x());

    layoutSystemElements(options, ctx, score, system);
    SystemLayout::layout2(system, ctx);     // compute staff distances
    for (MeasureBase* mb : system->measures()) {
        MeasureLayout::layoutCrossStaff(mb, ctx);
    }
    // TODO: now that the code at the top of this function does this same backwards search,
    // we might be able to eliminate this block
    // but, lc might be used elsewhere so we need to be careful
    measure = system->measures().back();

    if (measure) {
        measure = measure->findPotentialSectionBreak();
    }

    if (measure) {
        const LayoutBreak* layoutBreak = measure->sectionBreakElement();
        ctx.firstSystem        = measure->sectionBreak() && !options.isMode(LayoutMode::FLOAT);
        ctx.firstSystemIndent  = ctx.firstSystem && options.firstSystemIndent && layoutBreak->firstSystemIndentation();
        ctx.startWithLongNames = ctx.firstSystem && layoutBreak->startWithLongNames();
    }

    if (oldSystem && !(oldSystem->page() && oldSystem->page() != ctx.page)) {
        // We may have previously processed the ties of the next system (in LayoutChords::updateLineAttachPoints()).
        // We need to restore them to the correct state.
        SystemLayout::restoreTies(oldSystem);
    }

    return system;
}

void SystemLayout::justifySystem(System* system, double curSysWidth, double targetSystemWidth)
{
    double rest = targetSystemWidth - curSysWidth;
    if (RealIsNull(rest)) {
        return;
    }
    if (rest < 0) {
        LOGE("*** System justification error ***");
        return;
    }

    std::vector<Spring> springs;

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& s : toMeasure(mb)->segments()) {
            if (s.isChordRestType() && s.ticks() > Fraction(0, 1) && s.visible() && s.enabled() && !s.allElementsInvisible()) {
                double springConst = 1 / s.stretch();
                double width = s.width() - s.widthOffset();
                double preTension = width * springConst;
                springs.push_back(Spring(springConst, width, preTension, &s));
            }
        }
    }

    Segment::stretchSegmentsToWidth(springs, rest);

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* m = toMeasure(mb);
        m->respaceSegments();
    }
}

//---------------------------------------------------------
//   getNextSystem
//---------------------------------------------------------

System* SystemLayout::getNextSystem(LayoutContext& ctx)
{
    Score* score = ctx.score();
    bool isVBox = ctx.curMeasure->isVBox();
    System* system = nullptr;
    if (ctx.systemList.empty()) {
        system = Factory::createSystem(score->dummy()->page());
        ctx.systemOldMeasure = 0;
    } else {
        system = mu::takeFirst(ctx.systemList);
        ctx.systemOldMeasure = system->measures().empty() ? 0 : system->measures().back();
        system->clear();       // remove measures from system
    }
    score->systems().push_back(system);
    if (!isVBox) {
        size_t nstaves = score->Score::nstaves();
        system->adjustStavesNumber(nstaves);
        for (staff_idx_t i = 0; i < nstaves; ++i) {
            system->staff(i)->setShow(score->staff(i)->show());
        }
    }
    return system;
}

void SystemLayout::hideEmptyStaves(Score* score, System* system, bool isFirstSystem)
{
    size_t staves = score->nstaves();
    staff_idx_t staffIdx = 0;
    bool systemIsEmpty = true;

    for (Staff* staff : score->staves()) {
        SysStaff* ss  = system->staff(staffIdx);

        Staff::HideMode hideMode = staff->hideWhenEmpty();

        if (hideMode == Staff::HideMode::ALWAYS
            || (score->styleB(Sid::hideEmptyStaves)
                && (staves > 1)
                && !(isFirstSystem && score->styleB(Sid::dontHideStavesInFirstSystem))
                && hideMode != Staff::HideMode::NEVER)) {
            bool hideStaff = true;
            for (MeasureBase* m : system->measures()) {
                if (!m->isMeasure()) {
                    continue;
                }
                Measure* measure = toMeasure(m);
                if (!measure->isEmpty(staffIdx)) {
                    hideStaff = false;
                    break;
                }
            }
            // check if notes moved into this staff
            Part* part = staff->part();
            const size_t n = part->nstaves();
            if (hideStaff && (n > 1)) {
                staff_idx_t idx = part->staves().front()->idx();
                for (staff_idx_t i = 0; i < n; ++i) {
                    staff_idx_t st = idx + i;

                    for (MeasureBase* mb : system->measures()) {
                        if (!mb->isMeasure()) {
                            continue;
                        }
                        Measure* m = toMeasure(mb);
                        if (staff->hideWhenEmpty() == Staff::HideMode::INSTRUMENT && !m->isEmpty(st)) {
                            hideStaff = false;
                            break;
                        }
                        for (Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                            for (voice_idx_t voice = 0; voice < VOICES; ++voice) {
                                ChordRest* cr = s->cr(st * VOICES + voice);
                                int staffMove = cr ? cr->staffMove() : 0;
                                if (!cr || cr->isRest() || cr->staffMove() == 0) {
                                    // The case staffMove == 0 has already been checked by measure->isEmpty()
                                    continue;
                                }
                                if (staffIdx == st + staffMove) {
                                    hideStaff = false;
                                    break;
                                }
                            }
                        }
                        if (!hideStaff) {
                            break;
                        }
                    }
                    if (!hideStaff) {
                        break;
                    }
                }
            }
            ss->setShow(hideStaff ? false : staff->show());
            if (ss->show()) {
                systemIsEmpty = false;
            }
        } else if (!staff->show()) {
            // TODO: OK to check this first and not bother with checking if empty?
            ss->setShow(false);
        } else {
            systemIsEmpty = false;
            ss->setShow(true);
        }

        ++staffIdx;
    }
    Staff* firstVisible = nullptr;
    if (systemIsEmpty) {
        for (Staff* staff : score->staves()) {
            SysStaff* ss  = system->staff(staff->idx());
            if (staff->showIfEmpty() && !ss->show()) {
                ss->setShow(true);
                systemIsEmpty = false;
            } else if (!firstVisible && staff->show()) {
                firstVisible = staff;
            }
        }
    }
    // don’t allow a complete empty system
    if (systemIsEmpty && !score->staves().empty()) {
        Staff* staff = firstVisible ? firstVisible : score->staves().front();
        SysStaff* ss = system->staff(staff->idx());
        ss->setShow(true);
    }
    // Re-create the shapes to account for newly hidden or un-hidden staves
    for (auto mb : system->measures()) {
        if (mb->isMeasure()) {
            for (auto& seg : toMeasure(mb)->segments()) {
                seg.createShapes();
            }
        }
    }
}

void SystemLayout::layoutSystemElements(const LayoutOptions& options, LayoutContext& ctx, Score* score, System* system)
{
    if (score->noStaves()) {
        return;
    }

    //-------------------------------------------------------------
    //    create cr segment list to speed up computations
    //-------------------------------------------------------------

    std::vector<Segment*> sl;
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* m = toMeasure(mb);
        MeasureLayout::layoutMeasureNumber(m, ctx);
        MeasureLayout::layoutMMRestRange(m, ctx);

        // in continuous view, entire score is one system
        // but we only need to process the range
        if (options.isLinearMode() && (m->tick() < ctx.startTick || m->tick() > ctx.endTick)) {
            continue;
        }
        for (Segment* s = m->first(); s; s = s->next()) {
            if (s->isChordRestType() || !s->annotations().empty()) {
                sl.push_back(s);
            }
        }
    }

    //-------------------------------------------------------------
    // layout beams
    //  Needs to be done before creating skylines as stem lengths
    //  may change.
    //-------------------------------------------------------------

    for (Segment* s : sl) {
        if (!s->isChordRestType()) {
            continue;
        }
        BeamLayout::layoutNonCrossBeams(s, ctx);
        // Must recreate the shapes because stem lengths may have been changed!
        s->createShapes();
    }

    for (Segment* s : sl) {
        for (EngravingItem* item : s->elist()) {
            if (!item || !item->isRest()) {
                continue;
            }
            Rest* rest = toRest(item);
            Beam* beam = rest->beam();
            if (beam && !beam->cross()) {
                BeamLayout::verticalAdjustBeamedRests(rest, beam, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    //    create skylines
    //-------------------------------------------------------------

    for (size_t staffIdx = 0; staffIdx < score->nstaves(); ++staffIdx) {
        SysStaff* ss = system->staff(staffIdx);
        Skyline& skyline = ss->skyline();
        skyline.clear();
        for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }
            Measure* m = toMeasure(mb);
            MeasureNumber* mno = m->noText(staffIdx);
            MMRestRange* mmrr  = m->mmRangeText(staffIdx);
            // no need to build skyline outside of range in continuous view
            if (options.isLinearMode() && (m->tick() < ctx.startTick || m->tick() > ctx.endTick)) {
                continue;
            }
            if (mno && mno->addToSkyline()) {
                ss->skyline().add(mno->bbox().translated(m->pos() + mno->pos()));
            }
            if (mmrr && mmrr->addToSkyline()) {
                ss->skyline().add(mmrr->bbox().translated(m->pos() + mmrr->pos()));
            }
            if (m->staffLines(staffIdx)->addToSkyline()) {
                ss->skyline().add(m->staffLines(staffIdx)->bbox().translated(m->pos()));
            }
            for (Segment& s : m->segments()) {
                if (!s.enabled() || s.isTimeSigType()) {             // hack: ignore time signatures
                    continue;
                }
                PointF p(s.pos() + m->pos());
                if (s.segmentType()
                    & (SegmentType::BarLine | SegmentType::EndBarLine | SegmentType::StartRepeatBarLine | SegmentType::BeginBarLine)) {
                    BarLine* bl = toBarLine(s.element(staffIdx * VOICES));
                    if (bl && bl->addToSkyline()) {
                        RectF r = TLayout::layoutRect(bl, ctx);
                        skyline.add(r.translated(bl->pos() + p));
                    }
                } else {
                    track_idx_t strack = staffIdx * VOICES;
                    track_idx_t etrack = strack + VOICES;
                    for (EngravingItem* e : s.elist()) {
                        if (!e) {
                            continue;
                        }
                        track_idx_t effectiveTrack = e->vStaffIdx() * VOICES + e->voice();
                        if (effectiveTrack < strack || effectiveTrack >= etrack) {
                            continue;
                        }

                        // add element to skyline
                        if (e->addToSkyline()) {
                            skyline.add(e->shape().translated(e->pos() + p));
                            // add grace notes to skyline
                            if (e->isChord()) {
                                GraceNotesGroup& graceBefore = toChord(e)->graceNotesBefore();
                                GraceNotesGroup& graceAfter = toChord(e)->graceNotesAfter();
                                if (!graceBefore.empty()) {
                                    skyline.add(graceBefore.shape().translated(graceBefore.pos() + p));
                                }
                                if (!graceAfter.empty()) {
                                    skyline.add(graceAfter.shape().translated(graceAfter.pos() + p));
                                }
                            }
                            // If present, add ornament cue note to skyline
                            if (e->isChord()) {
                                Ornament* ornament = toChord(e)->findOrnament();
                                if (ornament) {
                                    Chord* cue = ornament->cueNoteChord();
                                    if (cue && cue->upNote()->visible()) {
                                        skyline.add(cue->shape().translate(cue->pos() + p));
                                    }
                                }
                            }
                        }

                        // add tremolo to skyline
                        if (e->isChord() && toChord(e)->tremolo()) {
                            Tremolo* t = toChord(e)->tremolo();
                            Chord* c1 = t->chord1();
                            Chord* c2 = t->chord2();
                            if (!t->twoNotes() || (c1 && !c1->staffMove() && c2 && !c2->staffMove())) {
                                if (t->chord() == e && t->addToSkyline()) {
                                    skyline.add(t->shape().translate(t->pos() + e->pos() + p));
                                }
                            }
                        }

                        // add beams to skline
                        if (e->isChordRest()) {
                            ChordRest* cr = toChordRest(e);
                            if (BeamLayout::isTopBeam(cr)) {
                                Beam* b = cr->beam();
                                b->addSkyline(skyline);
                            }
                        }
                    }
                }
            }
        }
    }

    //-------------------------------------------------------------
    // layout articulations, fingering and stretched bends
    //-------------------------------------------------------------

    for (Segment* s : sl) {
        for (EngravingItem* e : s->elist()) {
            if (!e || !e->isChord() || !score->staff(e->staffIdx())->show()) {
                continue;
            }
            Chord* c = toChord(e);
            ChordLayout::layoutArticulations(c, ctx);
            ChordLayout::layoutArticulations2(c, ctx);
            ChordLayout::layoutChordBaseFingering(c, system, ctx);
            for (Note* note : c->notes()) {
                for (EngravingItem* item : note->el()) {
                    if (item && item->isStretchedBend()) {
                        layout::v0::TLayout::layoutStretched(toStretchedBend(item), ctx);
                    }
                }
            }
        }
    }

    //-------------------------------------------------------------
    // layout tuplets
    //-------------------------------------------------------------

    std::map<track_idx_t, Fraction> skipTo;
    for (Segment* s : sl) {
        for (EngravingItem* e : s->elist()) {
            if (!e || !e->isChordRest() || !score->staff(e->staffIdx())->show()) {
                continue;
            }
            track_idx_t track = e->track();
            if (skipTo.count(track) && e->tick() < skipTo[track]) {
                continue; // don't lay out tuplets for this voice that have already been done
            }
            // find the top tuplet for this segment
            DurationElement* de = toChordRest(e);
            if (!de->tuplet()) {
                continue;
            }
            while (de->tuplet()) {
                de = de->tuplet();
            }
            TupletLayout::layout(de, ctx); // recursively lay out all tuplets covered by this tuplet

            // don't layout any tuplets covered by this top level tuplet for this voice--
            // they've already been laid out by layoutTuplet().
            skipTo[track] = de->tick() + de->ticks();
        }
    }

    //-------------------------------------------------------------
    // Drumline sticking
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isSticking()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // layout slurs
    //-------------------------------------------------------------

    bool useRange = false;    // TODO: lineMode();
    Fraction stick = useRange ? ctx.startTick : system->measures().front()->tick();
    Fraction etick = useRange ? ctx.endTick : system->measures().back()->endTick();
    auto spanners = score->spannerMap().findOverlapping(stick.ticks(), etick.ticks());

    // ties
    doLayoutTies(system, sl, stick, etick);

    // slurs
    std::vector<Spanner*> spanner;
    for (auto interval : spanners) {
        Spanner* sp = interval.value;
        sp->computeStartElement();
        sp->computeEndElement();
        ctx.processedSpanners.insert(sp);
        if (sp->tick() < etick && sp->tick2() >= stick) {
            if (sp->isSlur() && !toSlur(sp)->isCrossStaff()) {
                // skip cross-staff slurs, will be done after page layout
                spanner.push_back(sp);
            }
        }
    }
    processLines(system, spanner, false);
    for (auto s : spanner) {
        Slur* slur = toSlur(s);
        ChordRest* scr = s->startCR();
        ChordRest* ecr = s->endCR();
        if (scr && scr->isChord()) {
            ChordLayout::layoutArticulations3(toChord(scr), slur, ctx);
        }
        if (ecr && ecr->isChord()) {
            ChordLayout::layoutArticulations3(toChord(ecr), slur, ctx);
        }
    }

    //-------------------------------------------------------------
    // Fermata, TremoloBar
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isFermata() || e->isTremoloBar()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // Dynamics
    //-------------------------------------------------------------

    std::vector<Dynamic*> dynamics;
    for (Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isDynamic()) {
                Dynamic* d = toDynamic(e);
                TLayout::layout(d, ctx);
                if (d->autoplace()) {
                    d->manageBarlineCollisions();
                    d->autoplaceSegmentElement(false);
                    dynamics.push_back(d);
                }
            } else if (e->isFiguredBass()) {
                TLayout::layoutItem(e, ctx);
                e->autoplaceSegmentElement();
            }
        }
    }

    // add dynamics shape to skyline
    for (Dynamic* d : dynamics) {
        if (!d->addToSkyline()) {
            continue;
        }
        staff_idx_t si = d->staffIdx();
        Segment* s = d->segment();
        Measure* m = s->measure();
        system->staff(si)->skyline().add(d->shape().translate(d->pos() + s->pos() + m->pos()));
    }

    //-------------------------------------------------------------
    // Expressions
    // Must be done after dynamics. Remember that expressions may
    // also snap into alignment with dynamics.
    //-------------------------------------------------------------
    for (Segment* s : sl) {
        Measure* m = s->measure();
        for (EngravingItem* e : s->annotations()) {
            if (e->isExpression()) {
                TLayout::layoutItem(e, ctx);
                system->staff(e->staffIdx())->skyline().add(e->shape().translate(e->pos() + s->pos() + m->pos()));
            }
        }
    }

    //-------------------------------------------------------------
    // layout SpannerSegments for current system
    // voltas and tempo change lines are collected here, but laid out later
    //-------------------------------------------------------------

    spanner.clear();
    std::vector<Spanner*> hairpins;
    std::vector<Spanner*> ottavas;
    std::vector<Spanner*> pedal;
    std::vector<Spanner*> voltas;
    std::vector<Spanner*> tempoChangeLines;

    for (auto interval : spanners) {
        Spanner* sp = interval.value;
        if (sp->tick() < etick && sp->tick2() > stick) {
            if (sp->isOttava()) {
                if (sp->staff()->staffType()->isTabStaff()) {
                    continue;
                }

                ottavas.push_back(sp);
            } else if (sp->isPedal()) {
                pedal.push_back(sp);
            } else if (sp->isVolta()) {
                voltas.push_back(sp);
            } else if (sp->isHairpin()) {
                hairpins.push_back(sp);
            } else if (sp->isGradualTempoChange()) {
                tempoChangeLines.push_back(sp);
            } else if (!sp->isSlur() && !sp->isVolta()) {      // slurs are already
                spanner.push_back(sp);
            }
        }
    }
    processLines(system, hairpins, false);
    processLines(system, spanner, false);
    processLines(system, ottavas, false);
    processLines(system, pedal,   true);

    //-------------------------------------------------------------
    // Lyric
    //-------------------------------------------------------------

    LyricsLayout::layoutLyrics(options, score, system);

    // here are lyrics dashes and melisma
    for (Spanner* sp : score->unmanagedSpanners()) {
        if (sp->tick() >= etick || sp->tick2() <= stick) {
            continue;
        }
        TLayout::layoutSystem(sp, system, ctx);
    }

    //-------------------------------------------------------------
    // Harp pedal diagrams
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isHarpPedalDiagram()) {
                layout::v0::TLayout::layoutItem(e, ctx);
            }
        }
    }

    //
    // We need to known if we have FretDiagrams in the system to decide when to layout the Harmonies
    //

    bool hasFretDiagram = false;
    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isFretDiagram()) {
                hasFretDiagram = true;
                break;
            }
        }

        if (hasFretDiagram) {
            break;
        }
    }

    //-------------------------------------------------------------
    // Harmony, 1st place
    // If we have FretDiagrams, we want the Harmony above this and
    // above the volta, therefore we delay the layout.
    //-------------------------------------------------------------

    if (!hasFretDiagram) {
        HarmonyLayout::layoutHarmonies(sl, ctx);
        HarmonyLayout::alignHarmonies(system, sl, true, options.maxChordShiftAbove, options.maxChordShiftBelow);
    }

    //-------------------------------------------------------------
    // StaffText
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isStaffText()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // InstrumentChange
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isInstrumentChange()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // SystemText
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isPlayTechAnnotation() || e->isSystemText() || e->isTripletFeel()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // layout Voltas for current system
    //-------------------------------------------------------------

    processLines(system, voltas, false);

    //
    // vertical align volta segments
    //
    for (staff_idx_t staffIdx = 0; staffIdx < score->nstaves(); ++staffIdx) {
        std::vector<SpannerSegment*> voltaSegments;
        for (SpannerSegment* ss : system->spannerSegments()) {
            if (ss->isVoltaSegment() && ss->staffIdx() == staffIdx) {
                voltaSegments.push_back(ss);
            }
        }
        while (!voltaSegments.empty()) {
            // we assume voltas are sorted left to right (by tick values)
            double y = 0;
            int idx = 0;
            Volta* prevVolta = 0;
            for (SpannerSegment* ss : voltaSegments) {
                Volta* volta = toVolta(ss->spanner());
                if (prevVolta && prevVolta != volta) {
                    // check if volta is adjacent to prevVolta
                    if (prevVolta->tick2() != volta->tick()) {
                        break;
                    }
                }
                y = std::min(y, ss->ypos());
                ++idx;
                prevVolta = volta;
            }

            for (int i = 0; i < idx; ++i) {
                SpannerSegment* ss = voltaSegments[i];
                if (ss->autoplace() && ss->isStyled(Pid::OFFSET)) {
                    ss->setPosY(y);
                }
                if (ss->addToSkyline()) {
                    system->staff(staffIdx)->skyline().add(ss->shape().translate(ss->pos()));
                }
            }

            voltaSegments.erase(voltaSegments.begin(), voltaSegments.begin() + idx);
        }
    }

    //-------------------------------------------------------------
    // FretDiagram
    //-------------------------------------------------------------

    if (hasFretDiagram) {
        for (const Segment* s : sl) {
            for (EngravingItem* e : s->annotations()) {
                if (e->isFretDiagram()) {
                    TLayout::layoutItem(e, ctx);
                }
            }
        }

        //-------------------------------------------------------------
        // Harmony, 2nd place
        // We have FretDiagrams, we want the Harmony above this and
        // above the volta.
        //-------------------------------------------------------------

        HarmonyLayout::layoutHarmonies(sl, ctx);
        HarmonyLayout::alignHarmonies(system, sl, false, options.maxFretShiftAbove, options.maxFretShiftBelow);
    }

    //-------------------------------------------------------------
    // TempoText, tempo change lines
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isTempoText()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }
    processLines(system, tempoChangeLines, false);

    //-------------------------------------------------------------
    // Marker and Jump
    //-------------------------------------------------------------

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* m = toMeasure(mb);
        for (EngravingItem* e : m->el()) {
            if (e->isMarker() || e->isJump()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // RehearsalMark
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isRehearsalMark()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }

    //-------------------------------------------------------------
    // Image
    //-------------------------------------------------------------

    for (const Segment* s : sl) {
        for (EngravingItem* e : s->annotations()) {
            if (e->isImage()) {
                TLayout::layoutItem(e, ctx);
            }
        }
    }
}

void SystemLayout::doLayoutTies(System* system, std::vector<Segment*> sl, const Fraction& stick, const Fraction& etick)
{
    UNUSED(etick);

    for (Segment* s : sl) {
        for (EngravingItem* e : s->elist()) {
            if (!e || !e->isChord()) {
                continue;
            }
            Chord* c = toChord(e);
            for (Chord* ch : c->graceNotes()) {
                layoutTies(ch, system, stick);
            }
            layoutTies(c, system, stick);
        }
    }
}

void SystemLayout::processLines(System* system, std::vector<Spanner*> lines, bool align)
{
    std::vector<SpannerSegment*> segments;
    LayoutContext ctx(system->score());
    for (Spanner* sp : lines) {
        SpannerSegment* ss = TLayout::layoutSystem(sp, system, ctx);        // create/layout spanner segment for this system
        if (ss->autoplace()) {
            segments.push_back(ss);
        }
    }

    if (align && segments.size() > 1) {
        const size_t nstaves = system->staves().size();
        constexpr double minY = -1000000.0;
        const double defaultY = segments[0]->ypos();
        std::vector<double> y(nstaves, minY);

        for (SpannerSegment* ss : segments) {
            if (ss->visible()) {
                double& staffY = y[ss->staffIdx()];
                staffY = std::max(staffY, ss->ypos());
            }
        }
        for (SpannerSegment* ss : segments) {
            if (!ss->isStyled(Pid::OFFSET)) {
                continue;
            }
            const double staffY = y[ss->staffIdx()];
            if (staffY > minY) {
                ss->setPosY(staffY);
            } else {
                ss->setPosY(defaultY);
            }
        }
    }

    if (segments.size() > 1) {
        //how far vertically an endpoint should adjust to avoid other slur endpoints:
        const double slurCollisionVertOffset = 0.65 * system->spatium();
        const double slurCollisionHorizOffset = 0.2 * system->spatium();
        const double fuzzyHorizCompare = 0.25 * system->spatium();
        auto compare = [fuzzyHorizCompare](double x1, double x2) { return std::abs(x1 - x2) < fuzzyHorizCompare; };
        for (SpannerSegment* seg1 : segments) {
            if (!seg1->isSlurSegment()) {
                continue;
            }
            SlurSegment* slur1 = toSlurSegment(seg1);
            for (SpannerSegment* seg2 : segments) {
                if (!seg2->isSlurTieSegment() || seg1 == seg2) {
                    continue;
                }
                if (seg2->isSlurSegment()) {
                    SlurSegment* slur2 = toSlurSegment(seg2);
                    if (slur1->slur()->endChord() == slur2->slur()->startChord()
                        && compare(slur1->ups(Grip::END).p.y(), slur2->ups(Grip::START).p.y())) {
                        slur1->ups(Grip::END).p.rx() -= slurCollisionHorizOffset;
                        slur2->ups(Grip::START).p.rx() += slurCollisionHorizOffset;
                        slur1->computeBezier();
                        slur2->computeBezier();
                        continue;
                    }
                }
                SlurTieSegment* slurTie2 = toSlurTieSegment(seg2);

                // slurs don't collide with themselves or slurs on other staves
                if (slur1->vStaffIdx() != slurTie2->vStaffIdx()) {
                    continue;
                }
                // slurs which don't overlap don't need to be checked
                if (slur1->ups(Grip::END).p.x() < slurTie2->ups(Grip::START).p.x()
                    || slurTie2->ups(Grip::END).p.x() < slur1->ups(Grip::START).p.x()
                    || slur1->slur()->up() != slurTie2->slurTie()->up()) {
                    continue;
                }
                // START POINT
                if (compare(slur1->ups(Grip::START).p.x(), slurTie2->ups(Grip::START).p.x())) {
                    if (slur1->ups(Grip::END).p.x() > slurTie2->ups(Grip::END).p.x() || slurTie2->isTieSegment()) {
                        // slur1 is the "outside" slur
                        slur1->ups(Grip::START).p.ry() += slurCollisionVertOffset * (slur1->slur()->up() ? -1 : 1);
                        slur1->computeBezier();
                    }
                }
                // END POINT
                if (compare(slur1->ups(Grip::END).p.x(), slurTie2->ups(Grip::END).p.x())) {
                    // slurs have the same endpoint
                    if (slur1->ups(Grip::START).p.x() < slurTie2->ups(Grip::START).p.x() || slurTie2->isTieSegment()) {
                        // slur1 is the "outside" slur
                        slur1->ups(Grip::END).p.ry() += slurCollisionVertOffset * (slur1->slur()->up() ? -1 : 1);
                        slur1->computeBezier();
                    }
                }
            }
        }
    }
    //
    // Fix harmonic marks and vibrato overlaps
    //
    SpannerSegment* prevSegment = nullptr;
    bool fixed = false;

    for (SpannerSegment* ss : segments) {
        if (fixed) {
            fixed = false;
            prevSegment = ss;
            continue;
        }
        if (prevSegment) {
            if (prevSegment->visible()
                && ss->visible()
                && prevSegment->isHarmonicMarkSegment()
                && ss->isVibratoSegment()
                && RealIsEqual(prevSegment->x(), ss->x())) {
                double diff = ss->bbox().bottom() - prevSegment->bbox().bottom() + prevSegment->bbox().top();
                prevSegment->movePosY(diff);
                fixed = true;
            }
            if (prevSegment->visible()
                && ss->visible()
                && prevSegment->isVibratoSegment()
                && ss->isHarmonicMarkSegment()
                && RealIsEqual(prevSegment->x(), ss->x())) {
                double diff = prevSegment->bbox().bottom() - ss->bbox().bottom() + ss->bbox().top();
                ss->movePosY(diff);
                fixed = true;
            }
        }

        prevSegment = ss;
    }

    //
    // add shapes to skyline
    //
    for (SpannerSegment* ss : segments) {
        if (ss->addToSkyline()) {
            staff_idx_t stfIdx = ss->systemFlag() ? ss->staffIdxOrNextVisible() : ss->staffIdx();
            if (stfIdx == mu::nidx) {
                continue;
            }
            system->staff(stfIdx)->skyline().add(ss->shape().translate(ss->pos()));
        }
    }
}

void SystemLayout::layoutTies(Chord* ch, System* system, const Fraction& stick)
{
    SysStaff* staff = system->staff(ch->staffIdx());
    if (!staff->show()) {
        return;
    }
    for (Note* note : ch->notes()) {
        Tie* t = note->tieFor();
        if (t) {
            TieSegment* ts = SlurTieLayout::tieLayoutFor(t, system);
            if (ts && ts->addToSkyline()) {
                staff->skyline().add(ts->shape().translate(ts->pos()));
            }
        }
        t = note->tieBack();
        if (t) {
            if (t->startNote()->tick() < stick) {
                TieSegment* ts = SlurTieLayout::tieLayoutBack(t, system);
                if (ts && ts->addToSkyline()) {
                    staff->skyline().add(ts->shape().translate(ts->pos()));
                }
            }
        }
    }
}

/****************************************************************************
 * updateCrossBeams
 * Performs a pre-calculation of staff distances (final staff distances will
 * be calculated at the very end of layout) and updates the up() property
 * of cross-beam chords accordingly.
 * *************************************************************************/

void SystemLayout::updateCrossBeams(System* system, LayoutContext& ctx)
{
    SystemLayout::layout2(system, ctx); // Computes staff distances, essential for the rest of the calculations
    // Update grace cross beams
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& seg : toMeasure(mb)->segments()) {
            if (!seg.isChordRestType()) {
                continue;
            }
            for (EngravingItem* e : seg.elist()) {
                if (!e || !e->isChord()) {
                    continue;
                }
                for (Chord* grace : toChord(e)->graceNotes()) {
                    if (grace->beam() && (grace->beam()->cross() || grace->beam()->userModified())) {
                        ChordLayout::computeUp(grace, ctx);
                    }
                }
            }
        }
    }
    // Update normal chords cross beams and respective segments
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& seg : toMeasure(mb)->segments()) {
            for (EngravingItem* e : seg.elist()) {
                if (!e || !e->isChord()) {
                    continue;
                }
                Chord* chord = toChord(e);
                if (chord->beam() && (chord->beam()->cross() || chord->beam()->userModified())) {
                    bool prevUp = chord->up();
                    ChordLayout::computeUp(chord, ctx);
                    if (chord->up() != prevUp) {
                        // If the chord has changed direction needs to be re-laid out
                        ChordLayout::layoutChords1(chord->score(), &seg, chord->vStaffIdx(), ctx);
                        seg.createShape(chord->vStaffIdx());
                    }
                } else if (chord->tremolo() && chord->tremolo()->twoNotes()) {
                    Tremolo* t = chord->tremolo();
                    Chord* c1 = t->chord1();
                    Chord* c2 = t->chord2();
                    if (t->userModified() || (c1->staffMove() != 0 || c2->staffMove() != 0)) {
                        bool prevUp = chord->up();
                        ChordLayout::computeUp(chord, ctx);
                        if (chord->up() != prevUp) {
                            ChordLayout::layoutChords1(chord->score(), &seg, chord->vStaffIdx(), ctx);
                            seg.createShape(chord->vStaffIdx());
                        }
                    }
                }
            }
        }
    }
}

void SystemLayout::restoreTies(System* system)
{
    std::vector<Segment*> segList;
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& seg : toMeasure(mb)->segments()) {
            if (seg.isChordRestType()) {
                segList.push_back(&seg);
            }
        }
    }
    Fraction stick = system->measures().front()->tick();
    Fraction etick = system->measures().back()->endTick();
    doLayoutTies(system, segList, stick, etick);
}

void SystemLayout::manageNarrowSpacing(System* system, LayoutContext& ctx, double& curSysWidth, double targetSysWidth,
                                       const Fraction minTicks,
                                       const Fraction maxTicks)
{
    static constexpr double step = 0.2; // We'll try reducing the spacing in steps of 20%
                                        // (empiric compromise between looking good and not taking too many iterations)
    static constexpr double squeezeLimit = 0.3; // For some spaces, do not go below 30%

    // First, try to gradually reduce the duration stretch (i.e. flatten the spacing curve)
    double stretchCoeff = system->firstMeasure()->layoutStretch() - step;
    while (curSysWidth > targetSysWidth && RealIsEqualOrMore(stretchCoeff, 0.0)) {
        for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }
            Measure* m = toMeasure(mb);
            double prevWidth = m->width();
            MeasureLayout::computeWidth(m, ctx, minTicks, maxTicks, stretchCoeff, /*overrideMinMeasureWidth*/ true);
            curSysWidth += m->width() - prevWidth;
        }
        stretchCoeff -= step;
    }
    if (curSysWidth < targetSysWidth) {
        // Success!
        return;
    }

    // Now we are limited by the collision checks, so try to gradually squeeze everything without collisions
    staff_idx_t nstaves = system->score()->nstaves();
    double squeezeFactor = 1 - step;
    while (curSysWidth > targetSysWidth && RealIsEqualOrMore(squeezeFactor, 0.0)) {
        for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }

            // Reduce all paddings
            Measure* m = toMeasure(mb);
            double prevWidth = m->width();
            for (Segment& segment : m->segments()) {
                for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
                    Shape& shape = segment.staffShape(staffIdx);
                    shape.setSqueezeFactor(squeezeFactor);
                }
            }
            MeasureLayout::computeWidth(m, ctx, minTicks, maxTicks, stretchCoeff,  /*overrideMinMeasureWidth*/ true);

            // Reduce other distances that don't depend on paddings
            Segment* first = m->firstEnabled();
            double currentFirstX = first->x();
            if (currentFirstX > 0 && !first->hasAccidentals()) {
                first->setPosX(currentFirstX * std::max(squeezeFactor, squeezeLimit));
            }
            for (Segment& segment : m->segments()) {
                if (!segment.header() && !segment.isTimeSigType()) {
                    continue;
                }
                Segment* nextSeg = segment.next();
                if (!nextSeg || !nextSeg->isChordRestType()) {
                    continue;
                }
                double margin = segment.width() - segment.minHorizontalCollidingDistance(nextSeg);
                double reducedMargin = margin * (1 - std::max(squeezeFactor, squeezeLimit));
                segment.setWidth(segment.width() - reducedMargin);
            }
            m->respaceSegments();
            curSysWidth += m->width() - prevWidth;
        }
        squeezeFactor -= step;
    }
    if (curSysWidth < targetSysWidth) {
        // Success!
        return;
    }

    // Things don't fit without collisions, so give up and allow collisions
    double smallerStep = 0.25 * step;
    double widthReduction = 1 - smallerStep;
    while (curSysWidth > targetSysWidth && RealIsEqualOrMore(widthReduction, 0.0)) {
        for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }

            Measure* m = toMeasure(mb);
            double prevWidth = m->width();
            for (Segment& segment : m->segments()) {
                if (!segment.isChordRestType()) {
                    continue;
                }
                double curSegmentWidth = segment.width();
                segment.setWidth(curSegmentWidth * widthReduction);
            }
            m->respaceSegments();
            curSysWidth += m->width() - prevWidth;
        }
        widthReduction -= smallerStep;
    }
}

void SystemLayout::layoutSystem(System* system, LayoutContext& ctx, double xo1, const bool isFirstSystem, bool firstSystemIndent)
{
    if (system->_staves.empty()) {                 // ignore vbox
        return;
    }

    // Get standard instrument name distance
    double instrumentNameOffset = system->score()->styleMM(Sid::instrumentNameOffset);
    // Now scale it depending on the text size (which also may not follow staff scaling)
    double textSizeScaling = 1.0;
    double actualSize = 0.0;
    double defaultSize = 0.0;
    bool followStaffSize = true;
    if (ctx.startWithLongNames) {
        actualSize = system->score()->styleD(Sid::longInstrumentFontSize);
        defaultSize = DefaultStyle::defaultStyle().value(Sid::longInstrumentFontSize).toDouble();
        followStaffSize = system->score()->styleB(Sid::longInstrumentFontSpatiumDependent);
    } else {
        actualSize = system->score()->styleD(Sid::shortInstrumentFontSize);
        defaultSize = DefaultStyle::defaultStyle().value(Sid::shortInstrumentFontSize).toDouble();
        followStaffSize = system->score()->styleB(Sid::shortInstrumentFontSpatiumDependent);
    }
    textSizeScaling = actualSize / defaultSize;
    if (!followStaffSize) {
        textSizeScaling *= DefaultStyle::defaultStyle().value(Sid::spatium).toDouble() / system->score()->styleD(Sid::spatium);
    }
    textSizeScaling = std::max(textSizeScaling, 1.0);
    instrumentNameOffset *= textSizeScaling;

    size_t nstaves  = system->_staves.size();

    //---------------------------------------------------
    //  find x position of staves
    //---------------------------------------------------
    SystemLayout::layoutBrackets(system, ctx);
    double maxBracketsWidth = SystemLayout::totalBracketOffset(ctx);

    double maxNamesWidth = SystemLayout::instrumentNamesWidth(system, isFirstSystem, ctx);

    double indent = maxNamesWidth > 0 ? maxNamesWidth + instrumentNameOffset : 0.0;
    if (isFirstSystem && firstSystemIndent) {
        indent = std::max(indent, system->styleP(Sid::firstSystemIndentationValue) * system->mag() - maxBracketsWidth);
        maxNamesWidth = indent - instrumentNameOffset;
    }

    if (RealIsNull(indent)) {
        if (system->score()->styleB(Sid::alignSystemToMargin)) {
            system->_leftMargin = 0.0;
        } else {
            system->_leftMargin = maxBracketsWidth;
        }
    } else {
        system->_leftMargin = indent + maxBracketsWidth;
    }

    for (size_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        SysStaff* s  = system->_staves[staffIdx];
        Staff* staff = system->score()->staff(staffIdx);
        if (!staff->show() || !s->show()) {
            s->setbbox(RectF());
            continue;
        }

        double staffMag = staff->staffMag(Fraction(0, 1));         // ??? TODO
        int staffLines = staff->lines(Fraction(0, 1));
        if (staffLines <= 1) {
            double h = staff->lineDistance(Fraction(0, 1)) * staffMag * system->spatium();
            s->bbox().setRect(system->_leftMargin + xo1, -h, 0.0, 2 * h);
        } else {
            double h = (staffLines - 1) * staff->lineDistance(Fraction(0, 1));
            h = h * staffMag * system->spatium();
            s->bbox().setRect(system->_leftMargin + xo1, 0.0, 0.0, h);
        }
    }

    //---------------------------------------------------
    //  layout brackets
    //---------------------------------------------------

    system->setBracketsXPosition(xo1 + system->_leftMargin);

    //---------------------------------------------------
    //  layout instrument names x position
    //     at this point it is not clear which staves will
    //     be hidden, so layout all instrument names
    //---------------------------------------------------

    for (SysStaff* s : system->_staves) {
        for (InstrumentName* t : s->instrumentNames) {
            TLayout::layout(t, ctx);

            switch (t->align().horizontal) {
            case AlignH::LEFT:
                t->setPosX(0);
                break;
            case AlignH::HCENTER:
                t->setPosX(maxNamesWidth * .5);
                break;
            case AlignH::RIGHT:
                t->setPosX(maxNamesWidth);
                break;
            }
        }
    }

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* m = toMeasure(mb);
        if (m == system->measures().front() || (m->prev() && m->prev()->isHBox())) {
            MeasureLayout::createSystemBeginBarLine(m, ctx);
        }
    }
}

double SystemLayout::instrumentNamesWidth(System* system, bool isFirstSystem, LayoutContext& ctx)
{
    double namesWidth = 0.0;

    for (staff_idx_t staffIdx = 0; staffIdx < system->score()->nstaves(); ++staffIdx) {
        const SysStaff* staff = system->staff(staffIdx);
        if (!staff || (isFirstSystem && !staff->show())) {
            continue;
        }

        for (InstrumentName* name : staff->instrumentNames) {
            TLayout::layout(name, ctx);
            namesWidth = std::max(namesWidth, name->width());
        }
    }

    return namesWidth;
}

/// Calculates the total width of all brackets together that
/// would be visible when all staves are visible.
/// The logic in this method is closely related to the logic in
/// System::layoutBrackets and System::createBracket.
double SystemLayout::totalBracketOffset(LayoutContext& ctx)
{
    if (ctx.totalBracketsWidth >= 0) {
        return ctx.totalBracketsWidth;
    }

    size_t columns = 0;
    for (const Staff* staff : ctx.score()->staves()) {
        for (const BracketItem* bi : staff->brackets()) {
            columns = std::max(columns, bi->column() + 1);
        }
    }

    size_t nstaves = ctx.score()->nstaves();
    std::vector < double > bracketWidth(nstaves, 0.0);
    for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        const Staff* staff = ctx.score()->staff(staffIdx);
        for (auto bi : staff->brackets()) {
            if (bi->bracketType() == BracketType::NO_BRACKET || !bi->visible()) {
                continue;
            }

            //! This logic is partially copied from System::createBracket.
            //! Of course, we don't need to worry about invisible staves,
            //! but we do need to worry about brackets that span past the
            //! last staff.
            staff_idx_t firstStaff = staffIdx;
            staff_idx_t lastStaff = staffIdx + bi->bracketSpan() - 1;
            if (lastStaff >= nstaves) {
                lastStaff = nstaves - 1;
            }

            for (; firstStaff <= lastStaff; ++firstStaff) {
                if (ctx.score()->staff(firstStaff)->show()) {
                    break;
                }
            }
            for (; lastStaff >= firstStaff; --lastStaff) {
                if (ctx.score()->staff(lastStaff)->show()) {
                    break;
                }
            }

            size_t span = lastStaff - firstStaff + 1;
            if (span > 1
                || (bi->bracketSpan() == span)
                || (span == 1 && ctx.score()->styleB(Sid::alwaysShowBracketsWhenEmptyStavesAreHidden))) {
                Bracket* dummyBr = Factory::createBracket(ctx.score()->dummy(), /*isAccessibleEnabled=*/ false);
                dummyBr->setBracketItem(bi);
                dummyBr->setStaffSpan(firstStaff, lastStaff);
                TLayout::layout(dummyBr, ctx);
                for (staff_idx_t stfIdx = firstStaff; stfIdx <= lastStaff; ++stfIdx) {
                    bracketWidth[stfIdx] += dummyBr->width();
                }
                delete dummyBr;
            }
        }
    }

    double totalBracketsWidth = 0.0;
    for (double w : bracketWidth) {
        totalBracketsWidth = std::max(totalBracketsWidth, w);
    }
    ctx.totalBracketsWidth = totalBracketsWidth;

    return ctx.totalBracketsWidth;
}

double SystemLayout::layoutBrackets(System* system, const LayoutContext& ctx)
{
    size_t nstaves  = system->_staves.size();
    size_t columns = system->getBracketsColumnsCount();

#if (!defined (_MSCVER) && !defined (_MSC_VER))
    double bracketWidth[columns];
#else
    // MSVC does not support VLA. Replace with std::vector. If profiling determines that the
    //    heap allocation is slow, an optimization might be used.
    std::vector<double> bracketWidth(columns);
#endif
    for (size_t i = 0; i < columns; ++i) {
        bracketWidth[i] = 0.0;
    }

    std::vector<Bracket*> bl;
    bl.swap(system->_brackets);

    for (size_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        Staff* s = system->score()->staff(staffIdx);
        for (size_t i = 0; i < columns; ++i) {
            for (auto bi : s->brackets()) {
                if (bi->column() != i || bi->bracketType() == BracketType::NO_BRACKET) {
                    continue;
                }
                Bracket* b = SystemLayout::createBracket(system, ctx, bi, i, static_cast<int>(staffIdx), bl, system->firstMeasure());
                if (b != nullptr) {
                    bracketWidth[i] = std::max(bracketWidth[i], b->width());
                }
            }
        }
    }

    for (Bracket* b : bl) {
        delete b;
    }

    double totalBracketWidth = 0.0;

    if (!system->_brackets.empty()) {
        for (double w : bracketWidth) {
            totalBracketWidth += w;
        }
    }

    return totalBracketWidth;
}

void SystemLayout::addBrackets(System* system, Measure* measure, const LayoutContext& ctx)
{
    if (system->_staves.empty()) {                 // ignore vbox
        return;
    }

    size_t nstaves = system->_staves.size();

    //---------------------------------------------------
    //  find x position of staves
    //    create brackets
    //---------------------------------------------------

    size_t columns = system->getBracketsColumnsCount();

    std::vector<Bracket*> bl;
    bl.swap(system->_brackets);

    for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        Staff* s = system->score()->staff(staffIdx);
        for (size_t i = 0; i < columns; ++i) {
            for (auto bi : s->brackets()) {
                if (bi->column() != i || bi->bracketType() == BracketType::NO_BRACKET) {
                    continue;
                }
                SystemLayout::createBracket(system, ctx, bi, i, staffIdx, bl, measure);
            }
        }
        if (!system->staff(staffIdx)->show()) {
            continue;
        }
    }

    //---------------------------------------------------
    //  layout brackets
    //---------------------------------------------------

    system->setBracketsXPosition(measure->x());

    mu::join(system->_brackets, bl);
}

//---------------------------------------------------------
//   createBracket
//   Create a bracket if it spans more then one visible system
//   If measure is NULL adds the bracket in front of the system, else in front of the measure.
//   Returns the bracket if it got created, else NULL
//---------------------------------------------------------

Bracket* SystemLayout::createBracket(System* system, const LayoutContext& ctx, BracketItem* bi, size_t column, staff_idx_t staffIdx,
                                     std::vector<Bracket*>& bl,
                                     Measure* measure)
{
    size_t nstaves = system->_staves.size();
    staff_idx_t firstStaff = staffIdx;
    staff_idx_t lastStaff = staffIdx + bi->bracketSpan() - 1;
    if (lastStaff >= nstaves) {
        lastStaff = nstaves - 1;
    }

    for (; firstStaff <= lastStaff; ++firstStaff) {
        if (system->staff(firstStaff)->show()) {
            break;
        }
    }
    for (; lastStaff >= firstStaff; --lastStaff) {
        if (system->staff(lastStaff)->show()) {
            break;
        }
    }
    size_t span = lastStaff - firstStaff + 1;
    //
    // do not show bracket if it only spans one
    // system due to some invisible staves
    //
    if (span > 1
        || (bi->bracketSpan() == span)
        || (span == 1 && system->score()->styleB(Sid::alwaysShowBracketsWhenEmptyStavesAreHidden)
            && bi->bracketType() != BracketType::SQUARE)
        || (span == 1 && system->score()->styleB(Sid::alwaysShowSquareBracketsWhenEmptyStavesAreHidden)
            && bi->bracketType() == BracketType::SQUARE)) {
        //
        // this bracket is visible
        //
        Bracket* b = 0;
        track_idx_t track = staffIdx * VOICES;
        for (size_t k = 0; k < bl.size(); ++k) {
            if (bl[k]->track() == track && bl[k]->column() == column && bl[k]->bracketType() == bi->bracketType()
                && bl[k]->measure() == measure) {
                b = mu::takeAt(bl, k);
                break;
            }
        }
        if (b == 0) {
            b = Factory::createBracket(ctx.score()->dummy());
            b->setBracketItem(bi);
            b->setGenerated(true);
            b->setTrack(track);
            b->setMeasure(measure);
        }
        system->add(b);

        if (bi->selected()) {
            bool needSelect = true;

            std::vector<EngravingItem*> brackets = system->score()->selection().elements(ElementType::BRACKET);
            for (const EngravingItem* element : brackets) {
                if (toBracket(element)->bracketItem() == bi) {
                    needSelect = false;
                    break;
                }
            }

            if (needSelect) {
                system->score()->select(b, SelectType::ADD);
            }
        }

        b->setStaffSpan(firstStaff, lastStaff);

        return b;
    }

    return nullptr;
}

//---------------------------------------------------------
//   layout2
//    called after measure layout
//    adjusts staff distance
//---------------------------------------------------------

void SystemLayout::layout2(System* system, LayoutContext& ctx)
{
    Box* vb = system->vbox();
    if (vb) {
        TLayout::layout(vb, ctx);
        system->setbbox(vb->bbox());
        return;
    }

    system->setPos(0.0, 0.0);
    std::list<std::pair<size_t, SysStaff*> > visibleStaves;

    for (size_t i = 0; i < system->_staves.size(); ++i) {
        Staff* s  = system->score()->staff(i);
        SysStaff* ss = system->_staves[i];
        if (s->show() && ss->show()) {
            visibleStaves.push_back(std::pair<size_t, SysStaff*>(i, ss));
        } else {
            ss->setbbox(RectF());        // already done in layout() ?
        }
    }

    double _spatium            = system->spatium();
    double y                   = 0.0;
    double minVerticalDistance = system->score()->styleMM(Sid::minVerticalDistance);
    double staffDistance       = system->score()->styleMM(Sid::staffDistance);
    double akkoladeDistance    = system->score()->styleMM(Sid::akkoladeDistance);
    if (system->score()->enableVerticalSpread()) {
        staffDistance       = system->score()->styleMM(Sid::minStaffSpread);
        akkoladeDistance    = system->score()->styleMM(Sid::minStaffSpread);
    }

    if (visibleStaves.empty()) {
        return;
    }

    for (auto i = visibleStaves.begin();; ++i) {
        SysStaff* ss  = i->second;
        staff_idx_t si1 = i->first;
        Staff* staff  = system->score()->staff(si1);
        auto ni       = std::next(i);

        double dist = staff->height();
        double yOffset;
        double h;
        if (staff->lines(Fraction(0, 1)) == 1) {
            yOffset = _spatium * BARLINE_SPAN_1LINESTAFF_TO * 0.5;
            h = _spatium * (BARLINE_SPAN_1LINESTAFF_TO - BARLINE_SPAN_1LINESTAFF_FROM) * 0.5;
        } else {
            yOffset = 0.0;
            h = staff->height();
        }
        if (ni == visibleStaves.end()) {
            ss->setYOff(yOffset);
            ss->bbox().setRect(system->_leftMargin, y - yOffset, system->width() - system->_leftMargin, h);
            ss->saveLayout();
            break;
        }

        staff_idx_t si2 = ni->first;
        Staff* staff2  = system->score()->staff(si2);

        if (staff->part() == staff2->part()) {
            Measure* m = system->firstMeasure();
            double mag = m ? staff->staffMag(m->tick()) : 1.0;
            dist += akkoladeDistance * mag;
        } else {
            dist += staffDistance;
        }
        dist += staff2->userDist();
        bool fixedSpace = false;
        for (MeasureBase* mb : system->ml) {
            if (!mb->isMeasure()) {
                continue;
            }
            Measure* m = toMeasure(mb);
            Spacer* sp = m->vspacerDown(si1);
            if (sp) {
                if (sp->spacerType() == SpacerType::FIXED) {
                    dist = staff->height() + sp->gap();
                    fixedSpace = true;
                    break;
                } else {
                    dist = std::max(dist, staff->height() + sp->gap());
                }
            }
            sp = m->vspacerUp(si2);
            if (sp) {
                dist = std::max(dist, sp->gap() + staff->height());
            }
        }
        if (!fixedSpace) {
            // check minimum distance to next staff
            // note that in continuous view, we normally only have a partial skyline for the system
            // a full one is only built when triggering a full layout
            // therefore, we don't know the value we get from minDistance will actually be enough
            // so we remember the value between layouts and increase it when necessary
            // (the first layout on switching to continuous view gives us good initial values)
            // the result is space is good to start and grows as needed
            // it does not, however, shrink when possible - only by trigger a full layout
            // (such as by toggling to page view and back)
            double d = ss->skyline().minDistance(system->System::staff(si2)->skyline());
            if (system->score()->lineMode()) {
                double previousDist = ss->continuousDist();
                if (d > previousDist) {
                    ss->setContinuousDist(d);
                } else {
                    d = previousDist;
                }
            }
            dist = std::max(dist, d + minVerticalDistance);
        }
        ss->setYOff(yOffset);
        ss->bbox().setRect(system->_leftMargin, y - yOffset, system->width() - system->_leftMargin, h);
        ss->saveLayout();
        y += dist;
    }

    system->_systemHeight = system->staff(visibleStaves.back().first)->bbox().bottom();
    system->setHeight(system->_systemHeight);

    SystemLayout::setMeasureHeight(system, system->_systemHeight, ctx);

    //---------------------------------------------------
    //  layout brackets vertical position
    //---------------------------------------------------

    SystemLayout::layoutBracketsVertical(system, ctx);

    //---------------------------------------------------
    //  layout instrument names
    //---------------------------------------------------

    SystemLayout::layoutInstrumentNames(system);

    //---------------------------------------------------
    //  layout cross-staff slurs and ties
    //---------------------------------------------------

    Fraction stick = system->measures().front()->tick();
    Fraction etick = system->measures().back()->endTick();
    auto spanners = ctx.score()->spannerMap().findOverlapping(stick.ticks(), etick.ticks());

    std::vector<Spanner*> spanner;
    for (auto interval : spanners) {
        Spanner* sp = interval.value;
        if (sp->tick() < etick && sp->tick2() >= stick) {
            if (sp->isSlur()) {
                ChordRest* scr = sp->startCR();
                ChordRest* ecr = sp->endCR();
                staff_idx_t idx = sp->vStaffIdx();
                if (scr && ecr && (scr->vStaffIdx() != idx || ecr->vStaffIdx() != idx)) {
                    TLayout::layoutSystem(sp, system, ctx);
                }
            }
        }
    }
}

void SystemLayout::restoreLayout2(System* system, LayoutContext& ctx)
{
    if (system->vbox()) {
        return;
    }

    for (SysStaff* s : system->_staves) {
        s->restoreLayout();
    }

    system->setHeight(system->_systemHeight);
    SystemLayout::setMeasureHeight(system, system->_systemHeight, ctx);
}

void SystemLayout::setMeasureHeight(System* system, double height, LayoutContext& ctx)
{
    double _spatium = system->spatium();
    for (MeasureBase* m : system->ml) {
        if (m->isMeasure()) {
            // note that the factor 2 * _spatium must be corrected for when exporting
            // system distance in MusicXML (issue #24733)
            m->bbox().setRect(0.0, -_spatium, m->width(), height + 2.0 * _spatium);
        } else if (m->isHBox()) {
            m->bbox().setRect(0.0, 0.0, m->width(), height);
            TLayout::layout2(toHBox(m), ctx);
        } else if (m->isTBox()) {
            TLayout::layout(toTBox(m), ctx);
        } else {
            LOGD("unhandled measure type %s", m->typeName());
        }
    }
}

void SystemLayout::layoutBracketsVertical(System* system, LayoutContext& ctx)
{
    for (Bracket* b : system->_brackets) {
        int staffIdx1 = static_cast<int>(b->firstStaff());
        int staffIdx2 = static_cast<int>(b->lastStaff());
        double sy = 0;                           // assume bracket not visible
        double ey = 0;
        // if start staff not visible, try next staff
        while (staffIdx1 <= staffIdx2 && !system->_staves[staffIdx1]->show()) {
            ++staffIdx1;
        }
        // if end staff not visible, try prev staff
        while (staffIdx1 <= staffIdx2 && !system->_staves[staffIdx2]->show()) {
            --staffIdx2;
        }
        // if the score doesn't have "alwaysShowBracketsWhenEmptyStavesAreHidden" as true,
        // the bracket will be shown IF:
        // it spans at least 2 visible staves (staffIdx1 < staffIdx2) OR
        // it spans just one visible staff (staffIdx1 == staffIdx2) but it is required to do so
        // (the second case happens at least when the bracket is initially dropped)
        bool notHidden = system->score()->styleB(Sid::alwaysShowBracketsWhenEmptyStavesAreHidden)
                         ? (staffIdx1 <= staffIdx2) : (staffIdx1 < staffIdx2) || (b->span() == 1 && staffIdx1 == staffIdx2);
        if (notHidden) {                        // set vert. pos. and height to visible spanned staves
            sy = system->_staves[staffIdx1]->bbox().top();
            ey = system->_staves[staffIdx2]->bbox().bottom();
        }
        b->setPosY(sy);
        b->setHeight(ey - sy);
        TLayout::layout(b, ctx);
    }
}

void SystemLayout::layoutInstrumentNames(System* system)
{
    staff_idx_t staffIdx = 0;

    for (Part* p : system->score()->parts()) {
        SysStaff* s = system->staff(staffIdx);
        SysStaff* s2;
        size_t nstaves = p->nstaves();

        staff_idx_t visible = system->firstVisibleSysStaffOfPart(p);
        if (visible != mu::nidx) {
            // The top staff might be invisible but this top staff contains the instrument names.
            // To make sure these instrument name are drawn, even when the top staff is invisible,
            // move the InstrumentName elements to the first visible staff of the part.
            if (visible != staffIdx) {
                SysStaff* vs = system->staff(visible);
                for (InstrumentName* t : s->instrumentNames) {
                    t->setTrack(visible * VOICES);
                    t->setSysStaff(vs);
                    vs->instrumentNames.push_back(t);
                }
                s->instrumentNames.clear();
                s = vs;
            }

            for (InstrumentName* t : s->instrumentNames) {
                //
                // override Text->layout()
                //
                double y1, y2;
                switch (t->layoutPos()) {
                default:
                case 0:                         // center at part
                    y1 = s->bbox().top();
                    s2 = system->staff(staffIdx);
                    for (int i = static_cast<int>(staffIdx + nstaves - 1); i > 0; --i) {
                        SysStaff* s3 = system->staff(i);
                        if (s3->show()) {
                            s2 = s3;
                            break;
                        }
                    }
                    y2 = s2->bbox().bottom();
                    break;
                case 1:                         // center at first staff
                    y1 = s->bbox().top();
                    y2 = s->bbox().bottom();
                    break;
                case 2:                         // center between first and second staff
                    y1 = s->bbox().top();
                    y2 = system->staff(staffIdx + 1)->bbox().bottom();
                    break;
                case 3:                         // center at second staff
                    y1 = system->staff(staffIdx + 1)->bbox().top();
                    y2 = system->staff(staffIdx + 1)->bbox().bottom();
                    break;
                case 4:                         // center between first and second staff
                    y1 = system->staff(staffIdx + 1)->bbox().top();
                    y2 = system->staff(staffIdx + 2)->bbox().bottom();
                    break;
                case 5:                         // center at third staff
                    y1 = system->staff(staffIdx + 2)->bbox().top();
                    y2 = system->staff(staffIdx + 2)->bbox().bottom();
                    break;
                }
                t->setPosY(y1 + (y2 - y1) * .5 + t->offset().y());
            }
        }
        staffIdx += nstaves;
    }
}

void SystemLayout::setInstrumentNames(System* system, const LayoutContext& ctx, bool longName, Fraction tick)
{
    //
    // remark: add/remove instrument names is not undo/redoable
    //         as add/remove of systems is not undoable
    //
    if (system->vbox()) {                 // ignore vbox
        return;
    }
    if (!system->score()->showInstrumentNames()
        || (system->style()->styleB(Sid::hideInstrumentNameIfOneInstrument) && system->score()->visiblePartCount() <= 1)) {
        for (SysStaff* staff : system->_staves) {
            for (InstrumentName* t : staff->instrumentNames) {
                ctx.score()->removeElement(t);
            }
        }
        return;
    }

    int staffIdx = 0;
    for (SysStaff* staff : system->_staves) {
        Staff* s = system->score()->staff(staffIdx);
        Part* part = s->part();

        bool atLeastOneVisibleStaff = false;
        for (Staff* partStaff : part->staves()) {
            if (partStaff->show()) {
                atLeastOneVisibleStaff = true;
                break;
            }
        }

        bool showName = part->show() && atLeastOneVisibleStaff;
        if (!s->isTop() || !showName) {
            for (InstrumentName* t : staff->instrumentNames) {
                ctx.score()->removeElement(t);
            }
            ++staffIdx;
            continue;
        }

        const std::list<StaffName>& names = longName ? part->longNames(tick) : part->shortNames(tick);

        size_t idx = 0;
        for (const StaffName& sn : names) {
            InstrumentName* iname = mu::value(staff->instrumentNames, idx);
            if (iname == 0) {
                iname = new InstrumentName(system);
                // iname->setGenerated(true);
                iname->setParent(system);
                iname->setSysStaff(staff);
                iname->setTrack(staffIdx * VOICES);
                iname->setInstrumentNameType(longName ? InstrumentNameType::LONG : InstrumentNameType::SHORT);
                iname->setLayoutPos(sn.pos());
                ctx.score()->addElement(iname);
            }
            iname->setXmlText(sn.name());
            ++idx;
        }
        for (; idx < staff->instrumentNames.size(); ++idx) {
            ctx.score()->removeElement(staff->instrumentNames[idx]);
        }
        ++staffIdx;
    }
}
