#include <eosio.system/eosio.system.hpp>
#include <eosio.token/eosio.token.hpp>
// TELOS BEGIN
#include "system_kick.cpp"
#define MAX_PRODUCERS 23     // revised for TEDP 2 Phase 2, also set in system_rotation.cpp, change in both places
// TELOS END

namespace eosiosystem {

   using eosio::current_time_point;
   using eosio::microseconds;
   using eosio::token;

   void system_contract::onblock(ignore<block_header>)
   {
      using namespace eosio;

      require_auth(get_self());

      // Deserialize needed fields from block header.
      block_timestamp timestamp;
      name            producer;
      uint16_t        confirmed;
      checksum256     previous_block_id;

      _ds >> timestamp >> producer >> confirmed >> previous_block_id;
      (void)confirmed; // Only to suppress warning since confirmed is not used.

      // Add latest block information to blockinfo table.
      add_to_blockinfo_table(previous_block_id, timestamp);

      // _gstate2.last_block_num is not used anywhere in the system contract code anymore.
      // Although this field is deprecated, we will continue updating it for now until the last_block_num field
      // is eventually completely removed, at which point this line can be removed.
      _gstate2.last_block_num = timestamp;
      
      /** until activation, no new rewards are paid */
      // TELOS BEGIN
      _gstate.block_num++;
      if (_gstate.thresh_activated_stake_time == time_point()) {
          if(_gstate.block_num >= block_num_network_activation && _gstate.total_producer_vote_weight > 0) {
              _gstate.thresh_activated_stake_time = current_time_point();
              _gstate.last_claimrewards = timestamp.slot;
          }
          return;
      }
      // TELOS END

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();

      // TELOS BEGIN
      if(check_missed_blocks(timestamp, producer)) {
         update_missed_blocks_per_rotation();
         reset_schedule_metrics(producer);
         claimrewards_snapshot();
         _gstate.last_claimrewards = timestamp.slot; 
         _gstate.total_unpaid_blocks = 0;
      }

      // TELOS END

      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end() ) {
         _producers.modify( prod, same_payer, [&](auto& p ) {
            if(p.strike_count == 0){
               _gstate.total_unpaid_blocks++;
               p.unpaid_blocks++;   
            }
            p.lifetime_produced_blocks++; 
         });
      }

      recalculate_votes();  

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {


         update_elected_producers( timestamp );

         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {

            name_bid_table bids(get_self(), get_self().value);

            auto idx = bids.get_index<"highbid"_n>();

            auto highest = idx.lower_bound( std::numeric_limits<uint64_t>::max()/2 );
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                _gstate.thresh_activated_stake_time > time_point() &&
                (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
            ) {
               _gstate.last_name_close = timestamp;
               channel_namebid_to_rex( highest->high_bid );
               idx.modify( highest, same_payer, [&]( auto& b ){
                  b.high_bid = -b.high_bid;
               });
            }
         }

      }
        


      // TELOS END
   }



   

   void system_contract::claimrewards( const name& owner ) {
      require_auth( owner );

      const auto& prod = _producers.get( owner.value );
      check( prod.active(), "producer does not have an active key" );

      // TELOS BEGIN
      check( _gstate.thresh_activated_stake_time > time_point(),
              "cannot claim rewards until the chain is activated (1,000,000 blocks produced)");

      auto p = _payments.find(owner.value);
      check(p != _payments.end(), "No payment exists for account");
      auto prod_payment = *p;
      auto pay_amount = prod_payment.pay;

      //NOTE: consider resetting producer's last claim time to 0 here, instead of during snapshot.
      {
          token::transfer_action transfer_act{ token_account, { bpay_account, active_permission } };
          transfer_act.send( bpay_account, owner, pay_amount, "Producer/Standby Payment" );
      }

      _payments.erase(p);
      // TELOS END

      // TELOS BEGIN REDACTED, REST OF STOCK PAYMENT LOGIC MOVED TO claimrewards_snapshot
      /*
      const auto ct = current_time_point();

      check( ct - prod.last_claim_time > microseconds(useconds_per_day), "already claimed rewards within past day" );

      const asset token_supply   = token::get_supply(token_account, core_symbol().code() );
      const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();

      if( usecs_since_last_fill > 0 && _gstate.last_pervote_bucket_fill > time_point() ) {
         double additional_inflation = (_gstate4.continuous_rate * double(token_supply.amount) * double(usecs_since_last_fill)) / double(useconds_per_year);
         check( additional_inflation <= double(std::numeric_limits<int64_t>::max() - ((1ll << 10) - 1)),
                "overflow in calculating new tokens to be issued; inflation rate is too high" );
         int64_t new_tokens = (additional_inflation < 0.0) ? 0 : static_cast<int64_t>(additional_inflation);

         int64_t to_producers     = (new_tokens * uint128_t(pay_factor_precision)) / _gstate4.inflation_pay_factor;
         int64_t to_savings       = new_tokens - to_producers;
         int64_t to_per_block_pay = (to_producers * uint128_t(pay_factor_precision)) / _gstate4.votepay_factor;
         int64_t to_per_vote_pay  = to_producers - to_per_block_pay;

         if( new_tokens > 0 ) {
            {
               token::issue_action issue_act{ token_account, { {get_self(), active_permission} } };
               issue_act.send( get_self(), asset(new_tokens, core_symbol()), "issue tokens for producer pay and savings" );
            }
            {
               token::transfer_action transfer_act{ token_account, { {get_self(), active_permission} } };
               if( to_savings > 0 ) {
                  transfer_act.send( get_self(), saving_account, asset(to_savings, core_symbol()), "unallocated inflation" );
               }
               if( to_per_block_pay > 0 ) {
                  transfer_act.send( get_self(), bpay_account, asset(to_per_block_pay, core_symbol()), "fund per-block bucket" );
               }
               if( to_per_vote_pay > 0 ) {
                  transfer_act.send( get_self(), vpay_account, asset(to_per_vote_pay, core_symbol()), "fund per-vote bucket" );
               }
            }
         }

         _gstate.pervote_bucket          += to_per_vote_pay;
         _gstate.perblock_bucket         += to_per_block_pay;
         _gstate.last_pervote_bucket_fill = ct;
      }

      auto prod2 = _producers2.find( owner.value );

      /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and
      /// time duration the vote weight has been held into one metric.
      const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);

      bool crossed_threshold       = (last_claim_plus_3days <= ct);
      bool updated_after_threshold = true;
      if ( prod2 != _producers2.end() ) {
         updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
      } else {
         prod2 = _producers2.emplace( owner, [&]( producer_info2& info  ) {
            info.owner                     = owner;
            info.last_votepay_share_update = ct;
         });
      }

      // Note: updated_after_threshold implies cross_threshold (except if claiming rewards when the producers2 table row did not exist).
      // The exception leads to updated_after_threshold to be treated as true regardless of whether the threshold was crossed.
      // This is okay because in this case the producer will not get paid anything either way.
      // In fact it is desired behavior because the producers votes need to be counted in the global total_producer_votepay_share for the first time.

      int64_t producer_per_block_pay = 0;
      if( _gstate.total_unpaid_blocks > 0 ) {
         producer_per_block_pay = (_gstate.perblock_bucket * prod.unpaid_blocks) / _gstate.total_unpaid_blocks;
      }

      double new_votepay_share = update_producer_votepay_share( prod2,
                                    ct,
                                    updated_after_threshold ? 0.0 : prod.total_votes,
                                    true // reset votepay_share to zero after updating
                                 );

      int64_t producer_per_vote_pay = 0;
      if( _gstate2.revision > 0 ) {
         double total_votepay_share = update_total_votepay_share( ct );
         if( total_votepay_share > 0 && !crossed_threshold ) {
            producer_per_vote_pay = int64_t((new_votepay_share * _gstate.pervote_bucket) / total_votepay_share);
            if( producer_per_vote_pay > _gstate.pervote_bucket )
               producer_per_vote_pay = _gstate.pervote_bucket;
         }
      } else {
         if( _gstate.total_producer_vote_weight > 0 ) {
            producer_per_vote_pay = int64_t((_gstate.pervote_bucket * prod.total_votes) / _gstate.total_producer_vote_weight);
         }
      }

      if( producer_per_vote_pay < min_pervote_daily_pay ) {
         producer_per_vote_pay = 0;
      }

      _gstate.pervote_bucket      -= producer_per_vote_pay;
      _gstate.perblock_bucket     -= producer_per_block_pay;
      _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

      update_total_votepay_share( ct, -new_votepay_share, (updated_after_threshold ? prod.total_votes : 0.0) );

      _producers.modify( prod, same_payer, [&](auto& p) {
         p.last_claim_time = ct;
         p.unpaid_blocks   = 0;
      });

      if ( producer_per_block_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {bpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( bpay_account, owner, asset(producer_per_block_pay, core_symbol()), "producer block pay" );
      }
      if ( producer_per_vote_pay > 0 ) {
         token::transfer_action transfer_act{ token_account, { {vpay_account, active_permission}, {owner, active_permission} } };
         transfer_act.send( vpay_account, owner, asset(producer_per_vote_pay, core_symbol()), "producer vote pay" );
      }
      */
   }

   void system_contract::claimrewards_snapshot() {
        //check(_gstate.thresh_activated_stake_time > time_point(), "cannot take snapshot until chain is activated");

        //skips action, since there are no rewards to claim
        if (_gstate.total_unpaid_blocks <= 0) { 
            return;
        }

        auto ct = current_time_point();

        //const asset token_supply = eosio::token::get_supply(token_account, core_symbol().code() );
        const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();
        auto      to_producers          = 0;
        int64_t     issue_tokens          = 0;
            double bpay_rate = double(_gpayrate.bpay_rate) / double(100000); //NOTE: both bpay_rate and divisor were int64s which evaluated to 0. The divisor must be a double to get percentage.
            //auto to_workers = static_cast<int64_t>((12 * double(_gpayrate.worker_amount) * double(usecs_since_last_fill)) / double(useconds_per_year));
            //to_producers = static_cast<int64_t>((bpay_rate * double(token_supply.amount) * double(usecs_since_last_fill)) / double(useconds_per_year));
            to_producers = _gstate.total_unpaid_blocks * 0.1;
            to_producers = to_producers * 10000;
           
            //auto new_tokens = to_workers + to_producers;

            //NOTE: This line can cause failure if eosio.tedp doesn't have a balance emplacement
            //asset tedp_balance = eosio::token::get_balance(token_account, tedp_account, core_symbol().code());

            //int64_t transfer_tokens = 0;
            
            /* if (tedp_balance.amount > 0) {
                if (tedp_balance.amount >= new_tokens) {
                    transfer_tokens = new_tokens;
                } else {
                    transfer_tokens = tedp_balance.amount;
                    issue_tokens = new_tokens - transfer_tokens;
                }*/
            //} else {
               issue_tokens = to_producers;
            //}

            /* if (transfer_tokens > 0) {
                token::transfer_action transfer_act{ token_account, { tedp_account, active_permission } };
                transfer_act.send( tedp_account, get_self(), asset(transfer_tokens, core_symbol()), "TEDP: Inflation offset" );
            }*/

            //token::transfer_action transfer_act{ token_account, { get_self(), active_permission } };

            if (issue_tokens > 0) {

                token::issue_action issue_action{ token_account, { get_self(), active_permission }};
                issue_action.send(get_self(), asset(issue_tokens, symbol(symbol_code("HEP"), 4)), "Issue new TLOS tokens");

            }
               
            /* if (to_workers > 0) {
                transfer_act.send(get_self(), works_account, asset(to_workers, core_symbol()), "Transfer worker proposal share to works.decide account");
            }*/

            /* if (to_producers > 0) {
                transfer_act.send(get_self(), bpay_account, asset(to_producers, core_symbol()), "Transfer producer share to per-block bucket");
            }*/

            _gstate.last_pervote_bucket_fill = ct;
        

        //sort producers table
        auto sortedprods = _producers.get_index<"prototalvote"_n>();

        //calculate shares, based on MAX_PRODUCERS
        uint32_t activecount = 0;

        for (const auto &prod : sortedprods)
        {
            if (prod.active() && activecount < MAX_PRODUCERS)   //only count activated producers
                activecount++;
            else
                break;
        }


        int32_t index = 0;
        token::transfer_action transfer_act{token_account, {get_self(), active_permission}};
       // auto                   Divide = to_producers / 21; 
        for (const auto &prod : sortedprods) {
            if (!prod.active()) // skip inactive producers
                continue;

            if (prod.unpaid_blocks == 0)
               continue;

            if(prod.strike_count > 0)
               continue;
            
            print(prod.owner);

            auto pay_amount = (to_producers * prod.unpaid_blocks) / _gstate.total_unpaid_blocks;
            
            print(pay_amount);
            

            _producers.modify(prod, same_payer, [&](auto &p) {
                p.last_claim_time = ct;
                p.unpaid_blocks = 0;
            });

            // auto itr = _payments.find(prod.owner.value);

            // if (itr == _payments.end()) {
            //     _payments.emplace(_self, [&]( auto& a ) { 
            //         a.bp = prod.owner;
            //         a.pay = asset(pay_amount, core_symbol());
            //     });
            // } else //adds new payment to existing payment
            //     _payments.modify(itr, same_payer, [&]( auto& a ) {
            //         a.pay += asset(pay_amount, core_symbol());
            //     });

               transfer_act.send(get_self(), prod.owner, asset(pay_amount, symbol(symbol_code("HEP"), 4)),
                                 "Block Reward");

               
            
             
        }


        
    }

} //namespace eosiosystem
