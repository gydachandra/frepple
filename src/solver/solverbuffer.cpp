/***************************************************************************
 *                                                                         *
 * Copyright (C) 2007-2015 by frePPLe bvba                                 *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Affero General Public License as published   *
 * by the Free Software Foundation; either version 3 of the License, or    *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
 * GNU Affero General Public License for more details.                     *
 *                                                                         *
 * You should have received a copy of the GNU Affero General Public        *
 * License along with this program.                                        *
 * If not, see <http://www.gnu.org/licenses/>.                             *
 *                                                                         *
 ***************************************************************************/

#define FREPPLE_CORE
#include "frepple/solver.h"

namespace frepple {

/* @todo The flow quantity is handled at the wrong place. It needs to be
 * handled by the operation, since flows can exist on multiple suboperations
 * with different quantities. The buffer solve can't handle this, because
 * it only calls the solve() for the producing operation...
 * Are there some situations where the operation solver doesn't know enough
 * on the buffer behavior???
 */
void SolverCreate::solve(const Buffer* b, void* v) {
  // Call the user exit
  SolverData* data = static_cast<SolverData*>(v);
  if (userexit_buffer)
    userexit_buffer.call(b, PythonData(data->constrainedPlanning));

  // Verify the iteration limit isn't exceeded.
  if (getIterationMax() && ++data->iteration_count > getIterationMax()) {
    ostringstream ch;
    ch << "Maximum iteration count " << getIterationMax() << " exceeded";
    throw RuntimeException(ch.str());
  }

  // Safety stock planning is refactored to a separate method
  double requested_qty(data->state->q_qty);
  if (requested_qty == -1.0) {
    solveSafetyStock(b, v);
    return;
  }
  Date requested_date(data->state->q_date);
  bool tried_requested_date(false);

  // Message
  if (getLogLevel() > 1)
    logger << ++indentlevel << "Buffer '" << b->getName()
           << "' is asked: " << data->state->q_qty << "  "
           << data->state->q_date << endl;

  // Detect loops in the supply chain
  auto tmp_recent_buffers = data->recent_buffers;
  if (data->recent_buffers.contains(b)) {
    stringstream o;
    o << "Loop detected in the supply path: ";
    data->recent_buffers.echoSince(o, b);
    data->state->a_qty = 0.0;
    data->state->a_date = Date::infiniteFuture;
    if (data->logConstraints && data->planningDemand) {
      bool already_logged = false;
      for (auto t = data->planningDemand->getConstraints().begin();
           t != data->planningDemand->getConstraints().end(); ++t)
        if (t->getDescription() == o.str()) {
          already_logged = true;
          break;
        }
      if (!already_logged)
        data->planningDemand->getConstraints().push(new ProblemInvalidData(
            const_cast<Buffer*>(b), o.str(), "material", Date::infinitePast,
            Date::infiniteFuture, data->state->q_qty, false));
    }
    if (getLogLevel() > 1) {
      logger << indentlevel << "Warning: " << o.str() << endl;
      logger << indentlevel-- << "Buffer '" << b->getName()
             << "' answers: " << data->state->a_qty;
      if (!data->state->a_qty) logger << "  " << data->state->a_date;
      logger << endl;
    }
    return;
  } else
    data->recent_buffers.push(b);

  // Store the last command in the list, in order to undo the following
  // commands if required.
  auto topcommand = data->getCommandManager()->setBookmark();
  OperationPlan* prev_owner_opplan = data->state->curOwnerOpplan;

  // Evaluate the buffer profile and solve shortages by asking more material.
  // The loop goes from the requested date till the very end. Whenever the
  // event date changes, we evaluate if a shortage exists.
  Duration autofence = getAutoFence();
  Date currentDate;
  const TimeLine<FlowPlan>::Event* prev = nullptr;
  double shortage(0.0);
  Date extraSupplyDate(Date::infiniteFuture);
  Date extraInventoryDate(Date::infiniteFuture);
  Date noSupplyBefore(Date::infinitePast);
  double cumproduced =
      (b->getFlowPlans().rbegin() == b->getFlowPlans().end())
          ? 0
          : b->getFlowPlans().rbegin()->getCumulativeProduced();
  double current_minimum(0.0);
  double unconfirmed_supply(0.0);

  bool hasTransferbatching = false;
  for (auto fl = b->getFlows().begin(); fl != b->getFlows().end(); ++fl)
    if (fl->hasType<FlowTransferBatch>()) {
      hasTransferbatching = true;
      break;
    }

  for (auto cur = b->getFlowPlans().begin();; ++cur) {
    if (&*cur && cur->getEventType() == 1) {
      const FlowPlan* fplan = static_cast<const FlowPlan*>(&*cur);
      if (!fplan->getOperationPlan()->getActivated() &&
          fplan->getQuantity() > 0 &&
          fplan->getOperationPlan()->getOperation() !=
              b->getProducingOperation())
        unconfirmed_supply += fplan->getQuantity();
    }

    // Iterator has now changed to a new date or we have arrived at the end.
    // If multiple flows are at the same moment in time, we are not interested
    // in the inventory changes. It gets interesting only when a certain
    // inventory level remains unchanged for a certain time.
    if ((cur == b->getFlowPlans().end() || cur->getDate() > currentDate) &&
        prev) {
      // Some variables
      Date theDate = prev->getDate();
      double theOnHand = prev->getOnhand();
      double theDelta = theOnHand - current_minimum + shortage;

      // Evaluate the situation at the last flowplan before the date change.
      // Is there a shortage at that date?
      // We have 3 ways to resolve it:
      //  - Scan backward for a producer we can combine with to make a
      //    single batch.
      //  - Scan forward for producer we can replace in a single batch.
      //  - Create new supply for the shortage at that date.

      // Solution zero: wait for confirmed supply that is already existing
      bool supply_exists_already = false;
      if (theDelta < -ROUNDING_ERROR && autofence &&
          b->getOnHand(Date::infiniteFuture) > -ROUNDING_ERROR) {
        for (Buffer::flowplanlist::const_iterator scanner = cur;
             scanner != b->getFlowPlans().end() &&
             scanner->getDate() < theDate + autofence;
             ++scanner) {
          if (scanner->getQuantity() <= 0 ||
              scanner->getDate() < requested_date)
            continue;
          auto tmp = scanner->getOperationPlan();
          if (tmp && (tmp->getConfirmed() || tmp->getApproved())) {
            if (getLogLevel() > 1)
              logger << indentlevel
                     << "Refuse to create extra supply because confirmed "
                        "supply is already available at "
                     << scanner->getDate() << endl;
            supply_exists_already = true;
            if (shortage < -prev->getOnhand()) shortage = -prev->getOnhand();
            tried_requested_date = true;  // Disables an extra supply check
            break;
          }
        }
      }

      // Solution one: create supply at the shortage date itself
      if (theDelta < -ROUNDING_ERROR && !supply_exists_already) {
        // Can we get extra supply to solve the problem, or part of it?
        // If the shortage already starts before the requested date, it
        // was not created by the newly added flowplan, but existed before.
        // We don't consider this as a shortage for the current flowplan,
        // and we want our flowplan to try to repair the previous problems
        // if it can...
        bool loop = true;
        while (b->getProducingOperation() && theDate >= requested_date &&
               loop && (theDate >= noSupplyBefore || hasTransferbatching)) {
          // Create supply
          data->state->curBuffer = const_cast<Buffer*>(b);
          data->state->q_qty = -theDelta;
          data->state->q_date = theDate;

          // Check whether this date doesn't match with the requested date.
          // See a bit further why this is required.
          if (data->state->q_date == requested_date)
            tried_requested_date = true;

          // Make sure the new operationplans don't inherit an owner.
          // When an operation calls the solve method of suboperations, this
          // field is used to pass the information about the owner operationplan
          // down. When solving for buffers we must make sure NOT to pass owner
          // information. At the end of solving for a buffer we need to restore
          // the original settings...
          data->state->curOwnerOpplan = nullptr;

          // Note that the supply created with the next line changes the
          // onhand value at all later dates!
          b->getProducingOperation()->solve(*this, v);
          data->recent_buffers = tmp_recent_buffers;

          // Evaluate the reply date. The variable extraSupplyDate will store
          // the date when the producing operation tells us it can get extra
          // supply.
          if (data->state->a_date < extraSupplyDate &&
              data->state->a_date > requested_date)
            extraSupplyDate = data->state->a_date;

          // Prevent asking again at a time which we already know to be
          // infeasible
          noSupplyBefore = data->state->a_qty > ROUNDING_ERROR
                               ? Date::infinitePast
                               : data->state->a_date;

          //&& theDate >= noSupplyBefore
          // If we got some extra supply, we retry to get some more supply.
          // Only when no extra material is obtained, we give up.
          // When solving for safety stock or when the parameter allowsplit is
          // set to false we need to get a single replenishing operationplan.
          if (data->state->a_qty > ROUNDING_ERROR &&
              data->state->a_qty < -theDelta - ROUNDING_ERROR &&
              ((getAllowSplits() && !data->safety_stock_planning) ||
               data->state->a_qty ==
                   b->getProducingOperation()->getSizeMaximum()))
            theDelta += data->state->a_qty;
          else
            loop = false;
        }

        // Not enough supply was received to repair the complete problem
        if (prev && prev->getOnhand() + shortage < -ROUNDING_ERROR) {
          // Keep track of the shorted quantity.
          // Only consider shortages later than the requested date.
          if (theDate >= requested_date && shortage < -prev->getOnhand())
            shortage = -prev->getOnhand();

          // Reset the date from which excess material is in the buffer. This
          // excess material can be used to compute the date when the buffer
          // can be asked again for additional supply.
          extraInventoryDate = Date::infiniteFuture;
        }
      } else if (theDelta > unconfirmed_supply + ROUNDING_ERROR)
        // There is excess material at this date (coming from planned/frozen
        // material arrivals, surplus material created by lotsized operations,
        // etc...)
        // The unconfirmed_supply element is required to exclude any of the
        // excess inventory we may have caused ourselves. Such situations are
        // possible when there are loops in the supply chain.
        if (theDate > requested_date &&
            extraInventoryDate == Date::infiniteFuture)
          extraInventoryDate = theDate;
    }

    // We have reached the end of the flowplans. Breaking out of the loop
    // needs to be done here because in the next statements we are accessing
    // *cur, which isn't valid at the end of the list
    if (cur == b->getFlowPlans().end()) break;

    // The minimum has changed.
    // Note that these limits can be updated only after the processing of the
    // date change in the statement above. Otherwise the code above would
    // already use the new value before the intended date.
    // If the flag getPlanSafetyStockFirst is set, then we need to replenish
    // up to the minimum quantity. If it is not set (which is the default) then
    // we only replenish up to 0.
    if (cur->getEventType() == 3 &&
        (!data->buffer_solve_shortages_only || data->safety_stock_planning) &&
        !getShortagesOnly())
      current_minimum = cur->getMin();

    // Update the pointer to the previous flowplan.
    prev = &*cur;
    currentDate = cur->getDate();
  }

  // Note: the variable extraInventoryDate now stores the date from which
  // excess material is available in the buffer. The excess
  // We don't need to care how much material is lying there.

  // Check for supply at the requested date
  // Isn't this included in the normal loop? In some cases it is indeed, but
  // sometimes it isn't because in the normal loop there may still have been
  // onhand available and the shortage only shows at a later date than the
  // requested date.
  // E.g. Initial situation:              After extra consumer at time y:
  //      -------+                                --+
  //             |                                  |
  //             +------                            +---+
  //                                                    |
  //    0 -------y------                        0 --y---x-----
  //                                                    |
  //                                                    +-----
  // The first loop only checks for supply at times x and later. If it is not
  // feasible, we now check for supply at time y. It will create some extra
  // inventory, but at least the demand is met.
  // @todo The buffer solver could move backward in time from x till time y,
  // and try multiple dates. This would minimize the excess inventory created.
  while (shortage > ROUNDING_ERROR && b->getProducingOperation() &&
         !tried_requested_date) {
    // Create supply at the requested date
    data->state->curBuffer = const_cast<Buffer*>(b);
    data->state->q_qty = shortage;
    data->state->q_date = requested_date;
    // Make sure the new operationplans don't inherit an owner.
    // When an operation calls the solve method of suboperations, this field is
    // used to pass the information about the owner operationplan down. When
    // solving for buffers we must make sure NOT to pass owner information.
    // At the end of solving for a buffer we need to restore the original
    // settings...
    data->state->curOwnerOpplan = nullptr;
    // Note that the supply created with the next line changes the onhand value
    // at all later dates!
    // Note that asking at the requested date doesn't keep the material on
    // stock to a minimum.
    if (requested_qty - shortage < ROUNDING_ERROR)
      data->getCommandManager()->rollback(topcommand);
    b->getProducingOperation()->solve(*this, v);
    data->recent_buffers = tmp_recent_buffers;
    // Evaluate the reply
    if (data->state->a_date < extraSupplyDate &&
        data->state->a_date > requested_date)
      extraSupplyDate = data->state->a_date;
    if (data->state->a_qty > ROUNDING_ERROR)
      shortage -= data->state->a_qty;
    else
      tried_requested_date = true;
  }

  // Final evaluation of the replenishment
  if (data->constrainedPlanning && isConstrained()) {
    // Use the constrained planning result
    data->state->a_qty = requested_qty - shortage;
    if (data->state->a_qty < ROUNDING_ERROR) {
      data->getCommandManager()->rollback(topcommand);
      data->state->a_qty = 0.0;
    }
    data->state->a_date = (extraInventoryDate < extraSupplyDate)
                              ? extraInventoryDate
                              : extraSupplyDate;
    // Monitor as a constraint if there is no producing operation.
    // Note that if there is a producing operation the constraint is flagged
    // on the operation instead of on this buffer.
    if (!b->getProducingOperation() && data->logConstraints &&
        shortage > ROUNDING_ERROR && data->planningDemand)
      data->planningDemand->getConstraints().push(
          ProblemMaterialShortage::metadata, b, requested_date,
          Date::infiniteFuture, shortage);
  } else {
    // Enough inventory or supply available, or not material constrained.
    // In case of a plan that is not material constrained, the buffer tries to
    // solve for shortages as good as possible. Only in the end we 'lie' about
    // the result to the calling function. Material shortages will then remain
    // in the buffer.
    data->state->a_qty = requested_qty;
    data->state->a_date = Date::infiniteFuture;
  }

  // Restore the owning operationplan.
  data->state->curOwnerOpplan = prev_owner_opplan;
  data->recent_buffers = tmp_recent_buffers;

  // Reply quantity must be greater than 0
  assert(data->state->a_qty >= 0);

  // Increment the cost
  // Only the quantity consumed directly from the buffer is counted.
  // The cost of the material supply taken from producing operations is
  // computed seperately and not considered here.
  if (b->getItem() && data->state->a_qty > 0) {
    if (b->getFlowPlans().empty())
      cumproduced = 0.0;
    else
      cumproduced =
          b->getFlowPlans().rbegin()->getCumulativeProduced() - cumproduced;
    if (data->state->a_qty > cumproduced) {
      auto tmp = (data->state->a_qty - cumproduced) * b->getItem()->getCost();
      data->state->a_cost += tmp;
      if (data->logcosts && data->incostevaluation)
        logger << indentlevel << "     + cost on buffer '" << b << "': " << tmp
               << endl;
    }
  }

  // Message
  if (getLogLevel() > 1) {
    logger << indentlevel-- << "Buffer '" << b->getName()
           << "' answers: " << data->state->a_qty;
    if (!data->state->a_qty) logger << "  " << data->state->a_date;
    logger << endl;
  }
}

void SolverCreate::solveSafetyStock(const Buffer* b, void* v) {
  SolverData* data = static_cast<SolverData*>(v);
  auto shortagesonly = getShortagesOnly();

  // Message
  if (getLogLevel() > 1)
    logger << ++indentlevel << "Buffer '" << b->getName() << "' solves for "
           << (shortagesonly ? "shortages" : "safety stock") << endl;

  // Scan the complete horizon
  Date currentDate;
  const TimeLine<FlowPlan>::Event* prev = nullptr;
  double shortage(0.0);
  double current_minimum(0.0);
  Buffer::flowplanlist::const_iterator cur = b->getFlowPlans().begin();
  Calendar* alignment_cal = Plan::instance().getCalendar();
  while (true) {
    // Iterator has now changed to a new date or we have arrived at the end.
    // If multiple flows are at the same moment in time, we are not interested
    // in the inventory changes. It gets interesting only when a certain
    // inventory level remains unchanged for a certain time.
    if ((cur == b->getFlowPlans().end() || cur->getDate() > currentDate) &&
        prev) {
      // Some variables
      double theOnHand = prev->getOnhand();
      double theDelta = theOnHand - current_minimum + shortage;
      bool loop = true;

      if (alignment_cal) {
        // Adjust the requirement quantity to meet the full requirements of the
        // current plan.calendar bucket.
        Calendar::EventIterator bckt_end(alignment_cal, prev->getDate(), true);
        ++bckt_end;
        auto tmp_current_minimum = current_minimum;
        for (Buffer::flowplanlist::const_iterator f(prev);
             f != b->getFlowPlans().end() && f->getDate() < bckt_end.getDate();
             ++f) {
          if (f->getEventType() == 3 && !shortagesonly)
            tmp_current_minimum = f->getMin();
          if (f->isLastOnDate()) {
            auto tmp = f->getOnhand() - tmp_current_minimum + shortage;
            if (tmp < theDelta) theDelta = tmp;
          }
        }
      }

      // Evaluate the situation at the last flowplan before the date change.
      // Is there a shortage at that date?
      Date nextAskDate;
      unsigned int loopcounter = 20;  // Performance protection
      if (theDelta && b->getProducingOperation() &&
          b->getProducingOperation()->getSizeMaximum()) {
        double tmp =
            -theDelta / b->getProducingOperation()->getSizeMaximum() + 20;
        if (tmp > loopcounter) loopcounter = static_cast<unsigned int>(tmp);
      }
      while (theDelta < -ROUNDING_ERROR && b->getProducingOperation() && loop &&
             --loopcounter > 0) {
        // Create supply
        data->state->curBuffer = const_cast<Buffer*>(b);
        data->state->q_qty = -theDelta;
        data->state->q_date = nextAskDate ? nextAskDate : prev->getDate();

        // Validate whether confirmed/approved supply exists within the
        // autofence window
        if (getAutoFence()) {
          bool exists = false;
          for (auto f = b->getFlowPlans().begin(); f != b->getFlowPlans().end();
               ++f) {
            if (f->getQuantity() <= 0 || f->getDate() < data->state->q_date)
              continue;
            if (f->getDate() > data->state->q_date + getAutoFence()) break;
            auto tmp = f->getOperationPlan();
            if (tmp && (tmp->getConfirmed() || tmp->getApproved()) &&
                f->getDate() > data->state->q_date) {
              exists = true;
              break;
            }
          }
          if (exists) {
            // Not allowed to create extra supply at this moment
            loop = false;
            continue;
          }
        }

        // Make sure the new operationplans don't inherit an owner.
        // When an operation calls the solve method of suboperations, this field
        // is used to pass the information about the owner operationplan down.
        // When solving for buffers we must make sure NOT to pass owner
        // information. At the end of solving for a buffer we need to restore
        // the original settings...
        data->state->curOwnerOpplan = nullptr;

        // Note that the supply created with the next line changes the
        // onhand value at all later dates!
        auto topcommand = data->getCommandManager()->setBookmark();
        auto cur_q_date = data->state->q_date;
        data->state->q_qty_min = 1.0;
        data->recent_buffers.clear();
        data->recent_buffers.push(b);
        auto data_safety_stock_planning = data->safety_stock_planning;
        data->safety_stock_planning = false;
        auto data_buffer_solve_shortages_only =
            data->buffer_solve_shortages_only;
        data->buffer_solve_shortages_only = true;
        b->getProducingOperation()->solve(*this, v);
        data->safety_stock_planning = data_safety_stock_planning;
        data->buffer_solve_shortages_only = data_buffer_solve_shortages_only;

        if (data->state->a_qty > ROUNDING_ERROR)
          // If we got some extra supply, we retry to get some more supply.
          // Only when no extra material is obtained, we give up.
          theDelta += data->state->a_qty;
        else {
          data->getCommandManager()->rollback(topcommand);
          if ((cur != b->getFlowPlans().end() &&
               data->state->a_date < cur->getDate()) ||
              (cur == b->getFlowPlans().end() &&
               data->state->a_date < Date::infiniteFuture)) {
            if (data->state->a_date > cur_q_date)
              nextAskDate = data->state->a_date;
            else
              loop = false;
          } else
            loop = false;
        }
      }
    }

    // We have reached the end of the flowplans. Breaking out of the loop
    // needs to be done here because in the next statements we are accessing
    // *cur, which isn't valid at the end of the list
    if (cur == b->getFlowPlans().end()) break;

    // The minimum or the maximum have changed
    // Note that these limits can be updated only after the processing of the
    // date change in the statement above. Otherwise the code above would
    // already use the new value before the intended date.
    if (cur->getEventType() == 3 && !shortagesonly)
      current_minimum = cur->getMin();

    // Update the pointer to the previous flowplan.
    prev = &*cur;
    currentDate = cur->getDate();
    ++cur;
  }

  // Message
  if (getLogLevel() > 1)
    logger << indentlevel-- << "Buffer '" << b->getName() << "' solved for "
           << (shortagesonly ? "shortages" : "safety stock") << endl;
}

void SolverCreate::solve(const BufferInfinite* b, void* v) {
  SolverData* data = static_cast<SolverData*>(v);

  // Call the user exit
  if (userexit_buffer)
    userexit_buffer.call(b, PythonData(data->constrainedPlanning));

  // Message
  if (getLogLevel() > 1)
    logger << ++indentlevel << "Infinite buffer '" << b
           << "' is asked: " << data->state->q_qty << "  "
           << data->state->q_date << endl;

  // Reply whatever is requested, regardless of date, quantity or supply.
  // The demand is not propagated upstream either.
  data->state->a_qty = data->state->q_qty;
  data->state->a_date = data->state->q_date;
  if (b->getItem() && data->state->q_qty > 0) {
    auto tmp = data->state->q_qty * b->getItem()->getCost();
    data->state->a_cost += tmp;
    if (data->logcosts && data->incostevaluation)
      logger << indentlevel << "     + cost on buffer '" << b << "': " << tmp
             << endl;
  }

  // Message
  if (getLogLevel() > 1)
    logger << indentlevel-- << "Infinite buffer '" << b
           << "' answers: " << data->state->a_qty << endl;
}

}  // namespace frepple
