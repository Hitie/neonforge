#include "event_routes.hpp"
#include "test.hpp"
TEST(routes_preserve_order_and_snapshot_isolation)
{
    nf::EventRoutes routes;
    routes.add("test.event", 3);
    routes.add("test.event", 1);
    auto before = routes.snapshot("test.event");
    CHECK((*before == std::vector<nf_token>{1, 3}));
    routes.remove("test.event", 1);
    routes.add("test.event", 4);
    auto after = routes.snapshot("test.event");
    CHECK((*before == std::vector<nf_token>{1, 3}));
    CHECK((*after == std::vector<nf_token>{3, 4}));
    routes.remove("test.event", 3);
    routes.remove("test.event", 4);
    CHECK(!routes.snapshot("test.event"));
    CHECK((*after == std::vector<nf_token>{3, 4}));
}
TEST(unrelated_channels_do_not_change_a_topic_snapshot)
{
    nf::EventRoutes routes;
    routes.add("test.event", 1);
    auto expected = routes.snapshot("test.event");
    for (nf_token i = 2; i < 8194; ++i)
        routes.add("test.other." + std::to_string(i), i);
    auto actual = routes.snapshot("test.event");
    CHECK(actual == expected); // Steady-state dispatch must reuse its immutable token snapshot.
    CHECK(actual->size() == 1 && actual->front() == 1);
    CHECK(!routes.snapshot("test.missing"));
}
TEST(removing_the_last_channel_does_not_revive_an_old_snapshot)
{
    nf::EventRoutes routes;
    routes.add("test.event", 1);
    auto old = routes.snapshot("test.event");
    routes.remove("test.event", 1);
    routes.remove("test.missing", 1);
    routes.add("test.event", 2);
    CHECK((*old == std::vector<nf_token>{1}));
    CHECK((*routes.snapshot("test.event") == std::vector<nf_token>{2}));
}
int main() { return run_tests(); }
