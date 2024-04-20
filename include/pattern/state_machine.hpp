#include <iostream>
#include <vector>
#include <tuple>
#include <map>
#include <memory>
#include <functional>

namespace pattern {

using EventId = uint16_t;
using StateId = uint16_t;
using RuleId  = uint32_t;
using Func    = std::function<void(EventId)>;
using OnEnter = std::function<void(EventId)>;
using OnExit  = std::function<void(EventId)>;
using OnCheck = std::function<bool(EventId)>;

class StateMachine {
        inline static constexpr uint16_t NONE{0};
        StateId cur_state_{NONE};
        StateId next_state_{NONE};
        std::map<StateId, std::string> state_info_;
        std::map<EventId, std::string> event_info_;
        std::map<StateId, std::pair<OnEnter, OnExit>> state_;
        std::map<RuleId, std::vector<std::tuple<StateId, Func, OnCheck>>> rules_;
    public:
        StateMachine() = default;

        StateId GetCurState() const {
            return cur_state_;
        }
        const auto &GetCurStateInfo() const {
            return state_info_;
        }

        void AddRule(StateId src_state, StateId dst_state, EventId event, const std::string &event_name, Func &&on_changed, OnCheck &&cond_check) {
            event_info_[event] = event_name;
            RuleId rule_id = (src_state << 16) + event;
            rules_[rule_id].push_back({dst_state, on_changed, cond_check});
        }

        void AddState(StateId state, const std::string &state_name, OnEnter &&enter_func, OnExit &&exit_func) {
            state_[state] = {enter_func, exit_func};
            state_info_[state] = state_name;
        }

        void ProcessEvent(EventId event) {
            RuleId rule_id = (cur_state_ << 16) + event;
            if (auto iter = rules_.find(rule_id); iter != rules_.end()) {
                for (int i=0; i<iter->second.size(); i++) {
                    auto [dst_state, func, checker] = iter->second[i];
                    if (!checker || checker(event)) {
                        func(event);
                        StateChange(dst_state, event);
                        break;
                    }
                }
            } else {
                std::cout << "Not found corresponding event, event id : " << event << "\n";
            }
        }

    private:
        void StateChange(StateId next_state, EventId event) {
            if (state_.count(cur_state_) && state_.count(next_state)) {
                state_[cur_state_].second(event);
                state_[next_state].first(event);
                cur_state_ = next_state;
            } else {
                std::cout << "Not found corresponding state, [" << cur_state_ << "->" << next_state << "]" << std::endl;
            }
        }
};

}   /// end of namespace pattern

#ifdef TEST
enum State : uint16_t {
    STATE_A,
    STATE_B,
    STATE_C
};
enum Event : uint16_t {
    EVENT_A_2_B,
    EVENT_B_2_C,
    EVENT_C_2_A,
    EVENT_C_2_B
};

using namespace pattern;
class Test {
        pattern::StateMachine stm;
    public:
        Test() {
            InitRule();
            InitState();
        }

        void InitRule() {
            stm.AddRule(STATE_A, STATE_B, EVENT_A_2_B, "event_a_b", [this] (EventId) { std::cout << "state a-b callback\n"; }, nullptr);
            stm.AddRule(STATE_B, STATE_C, EVENT_B_2_C, "event_b_c", [this] (EventId) { std::cout << "state b-c callback\n"; }, nullptr);
            stm.AddRule(STATE_C, STATE_A, EVENT_C_2_A, "event_c_a", [this] (EventId) { std::cout << "state c-a callback\n"; }, nullptr);
        }

        void InitState() {
            stm.AddState(STATE_A, "state_a", [this] (EventId) { std::cout << "State A entered\n"; }, [this] (EventId) { std::cout << "State A exited\n"; });
            stm.AddState(STATE_B, "state_b", [this] (EventId) { std::cout << "State B entered\n"; }, [this] (EventId) { std::cout << "State B exited\n"; });
            stm.AddState(STATE_C, "state_c", [this] (EventId) { std::cout << "State C entered\n"; }, [this] (EventId) { std::cout << "State C exited\n"; });
        }

        void ProcessEvent(pattern::EventId event) {
            stm.ProcessEvent(event);
        }
};

int main() {
    std::shared_ptr<Test> test_ptr(new Test());
    std::cout << "Event A-B occur:\n\t";
    test_ptr->ProcessEvent(Event::EVENT_A_2_B);     /// State change success
    std::cout << "Event B-C occur:\n\t";
    test_ptr->ProcessEvent(Event::EVENT_B_2_C);     /// State change success
    std::cout << "Event C-B occur:\n\t";
    test_ptr->ProcessEvent(Event::EVENT_C_2_B);     /// Not found corresponding event

    return 0;
}
#endif