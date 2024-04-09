#include <eosio.system/eosio.system.hpp>
#include <string>
#define TWELVE_HOURS_US 43200000000
#define SIX_HOURS_US 21600000000
#define ONE_HOUR_US 900000000       // debug version
#define SIX_MINUTES_US 360000000    // debug version
#define TWELVE_MINUTES_US 720000000 // debug version
#define MAX_PRODUCERS 23     // revised for TEDP 2 Phase 2, also set in producer_pay.cpp, change in both places
#define TOP_PRODUCERS 21
#define MAX_VOTE_PRODUCERS 30

namespace eosiosystem {
using namespace eosio;

void system_contract::set_bps_rotation(name bpOut, name sbpIn) {
  _grotation.bp_currently_out = bpOut;
  _grotation.sbp_currently_in = sbpIn;
}

void system_contract::update_rotation_time(block_timestamp block_time) {
  _grotation.last_rotation_time = block_time;
  _grotation.next_rotation_time = block_timestamp(
      block_time.to_time_point() + time_point(microseconds(TWELVE_HOURS_US)));
}

void system_contract::update_missed_blocks_per_rotation() {
    //둘 사이의 거리를 계산 하는 것으로 _gschedule_metrics.producers_metric의 수를 계산
  auto active_schedule_size =
    std::distance(_gschedule_metrics.producers_metric.begin(),
                    _gschedule_metrics.producers_metric.end());
  //max_kick_bps에 위에서 구한 활성화된 프로듀서 수를 7로 나누어 대입
  uint16_t max_kick_bps = uint16_t(active_schedule_size / 7);
  //프로듀서 객체 리스트를 생성
  std::vector<producer_info> prods;
  //활성화된 프로듀서 수 만큼 반복문을 실행
  for (auto &pm : _gschedule_metrics.producers_metric) {
      //프로듀서 이름으로 프로듀서를 객체를 새로 생성
    auto pitr = _producers.find(pm.bp_name.value);
      //const char* pitr_char = nameToChar(pm.bp_name.value);
   // jjy_log(pitr_char);
    //해당 프로듀서가 컨테이너에 들어있는지와 활성화 상태인지 확인, 안들어 있고 활성화 상태라면 조건에 충족
    if (pitr != _producers.end() && pitr->is_active) {

        //해당 프로듀서가 블록을 누락한 적이 있는지 체크
        
      if (pm.missed_blocks_per_cycle > 0) {

         std::string missed_blocks_per_cycle_string = std::to_string(pm.missed_blocks_per_cycle);
          const char* missed_blocks_per_cycle_value = missed_blocks_per_cycle_string.c_str();

        //  print("\nblock producer: ", name{pm.name}, " missed ",
        //  pm.missed_blocks_per_cycle, " blocks.");
        _producers.modify(pitr, same_payer, [&](auto &p) {
          p.strike_count++;
          std::string strike_count_string = std::to_string(p.strike_count);
          const char* strike_count_value = strike_count_string.c_str();

          p.missed_blocks_per_rotation += pm.missed_blocks_per_cycle;
          p.total_punished += pm.missed_blocks_per_cycle * 2;

          std::string total_punished_string = std::to_string(p.total_punished);
          const char* total_punished_value = total_punished_string.c_str();

          //   print("\ntotal missed blocks: ", p.missed_blocks_per_rotation);
            std::string missed_blocks_per_rotation_string = std::to_string(p.missed_blocks_per_rotation);
            const char* missed_blocks_per_rotation_value  = missed_blocks_per_rotation_string.c_str();

        });
        
      }else if(pm.missed_blocks_per_cycle <= 0 && pitr->strike_count > 3){
        _producers.modify(pitr, same_payer, [&](auto &p) {
          p.punished_round++;

          std::string punished_round_string = std::to_string(pitr->punished_round);
          const char* punished_round_value = punished_round_string.c_str();

          std::string total_punished_string = std::to_string(pitr->total_punished);
          const char* total_punished_value = total_punished_string.c_str();

        });
          if(pitr->punished_round >= pitr->total_punished){

            _producers.modify(pitr, same_payer, [&](auto &p) {
              p.strike_count = 0;
              p.punished_round = 0;
              p.total_punished = 0;
            });
            
          }
      }

      if (pitr->missed_blocks_per_rotation > 0)
        prods.emplace_back(*pitr);
    }
  }

  std::sort(prods.begin(), prods.end(), [](const producer_info &p1,
                                           const producer_info &p2) {
    if (p1.missed_blocks_per_rotation != p2.missed_blocks_per_rotation)
      return p1.missed_blocks_per_rotation > p2.missed_blocks_per_rotation;
    else
      return p1.total_votes < p2.total_votes;
  });

  for (auto &prod : prods) {
    auto pitr = _producers.find(prod.owner.value);
    if(pitr->strike_count >= 3 && max_kick_bps > 0){
        _producers.modify(pitr, same_payer, [&](auto &p) {
          p.lifetime_missed_blocks += p.missed_blocks_per_rotation;
          p.strike_count = 0;
          p.punished_round = 0;
          p.total_punished = 0;
          p.kick(kick_type::REACHED_TRESHOLD);
      });
      max_kick_bps--;
    }else
     break;
    /*if (crossed_missed_blocks_threshold(pitr->missed_blocks_per_rotation,
                                        uint32_t(active_schedule_size)) &&
        max_kick_bps > 0) {
      _producers.modify(pitr, same_payer, [&](auto &p) {
        p.lifetime_missed_blocks += p.missed_blocks_per_rotation;
        p.kick(kick_type::REACHED_TRESHOLD);
      });
      max_kick_bps--;
    } else
      break;*/
  }
}

void system_contract::restart_missed_blocks_per_rotation(
    std::vector<producer_location_pair> prods) {
  // restart all missed blocks to bps and sbps
  for (size_t i = 0; i < prods.size(); i++) {
    auto bp_name = prods[i].first.producer_name;
    auto pitr = _producers.find(bp_name.value);

    if (pitr != _producers.end()) {
      _producers.modify(pitr, same_payer, [&](auto &p) {
        if (p.times_kicked > 0 && p.missed_blocks_per_rotation == 0) {
          p.times_kicked--;
        }
        p.lifetime_missed_blocks += p.missed_blocks_per_rotation;
        p.missed_blocks_per_rotation = 0;
      });
    }
  }
}

 bool system_contract::is_in_range(int32_t index, int32_t low_bound, int32_t up_bound) {
     return index >= low_bound && index < up_bound;
   } 

std::vector<producer_location_pair> system_contract::check_rotation_state( std::vector<producer_location_pair> prods, block_timestamp block_time) {
      uint32_t total_active_voted_prods = prods.size(); 
      std::vector<producer_location_pair>::iterator it_bp = prods.end();
      std::vector<producer_location_pair>::iterator it_sbp = prods.end();

      if (_grotation.next_rotation_time <= block_time) {

        if (total_active_voted_prods > TOP_PRODUCERS) {
          _grotation.bp_out_index = _grotation.bp_out_index >= TOP_PRODUCERS - 1 ? 0 : _grotation.bp_out_index + 1;
          _grotation.sbp_in_index = _grotation.sbp_in_index >= total_active_voted_prods - 1 ? TOP_PRODUCERS : _grotation.sbp_in_index + 1;

          name bp_name = prods[_grotation.bp_out_index].first.producer_name;
          name sbp_name = prods[_grotation.sbp_in_index].first.producer_name;

          it_bp = prods.begin() + int32_t(_grotation.bp_out_index);
          it_sbp = prods.begin() + int32_t(_grotation.sbp_in_index);

          set_bps_rotation(bp_name, sbp_name);
        } 

        update_rotation_time(block_time);
        restart_missed_blocks_per_rotation(prods);
      }
      else {
        if(_grotation.bp_currently_out != name(0) && _grotation.sbp_currently_in != name(0)) {
          auto bp_name = _grotation.bp_currently_out;
          it_bp = std::find_if(prods.begin(), prods.end(), [&bp_name](const producer_location_pair &g) {
            return g.first.producer_name == bp_name; 
          });

          auto sbp_name = _grotation.sbp_currently_in;
          it_sbp = std::find_if(prods.begin(), prods.end(), [&sbp_name](const producer_location_pair &g) {
            return g.first.producer_name == sbp_name; 
          });
          auto _bp_index = std::distance(prods.begin(), it_bp);
          auto _sbp_index = std::distance(prods.begin(), it_sbp);

          if(it_bp == prods.end() || it_sbp == prods.end()) {
              set_bps_rotation(name(0), name(0));

            if(total_active_voted_prods < TOP_PRODUCERS) {
              _grotation.bp_out_index = TOP_PRODUCERS;
              _grotation.sbp_in_index = MAX_PRODUCERS+1;
            }
          } else if (total_active_voted_prods > TOP_PRODUCERS && 
                    (!is_in_range(_bp_index, 0, TOP_PRODUCERS) || !is_in_range(_sbp_index, TOP_PRODUCERS, MAX_PRODUCERS))) {
              set_bps_rotation(name(0), name(0));
              it_bp = prods.end();
              it_sbp = prods.end();
          }
        }
    }

      std::vector<producer_location_pair>  top_producers;

      //Rotation
      if(it_bp != prods.end() && it_sbp != prods.end()) {
        for ( auto pIt = prods.begin(); pIt != prods.end(); ++pIt) {
          auto i = std::distance(prods.begin(), pIt); 
          // print("\ni-> ", i);
          if(i > TOP_PRODUCERS - 1) break;

          if(pIt->first.producer_name == it_bp->first.producer_name) {
            // print("\nprod sbp added to schedule -> ", name{it_sbp->producer_name});
            top_producers.emplace_back(*it_sbp);
          } else {
            // print("\nprod bp added to schedule -> ", name{pIt->producer_name});
            top_producers.emplace_back(*pIt);
          } 
        }
      } 
      else {
        top_producers = prods;
        if(prods.size() > TOP_PRODUCERS) top_producers.resize(TOP_PRODUCERS);
        else top_producers.resize(prods.size());
      }

  return top_producers;
}
}
