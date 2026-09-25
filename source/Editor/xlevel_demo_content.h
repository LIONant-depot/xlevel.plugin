#pragma once

namespace xlevel
{
    // xlioncore::transform (LIONCore.dll) owns world pose now - xlevel::transform retired.

    // Two trivial demo Update systems - LevelEditor otherwise registers ZERO systems (RegisterSystems<>() is
    // called empty, purely to lock component bit IDs), so the new System Registry panel/Play-Stop
    // toggle would have nothing real to list/reorder/enable/observe without these. Each just
    // printf's once per tick (flushed unconditionally, per this project's persistent-diagnostic-
    // logging preference) since LevelEditor has no viewport to observe a "real" effect through - reordering
    // them in the System Registry panel changes which line prints first; disabling one stops its own
    // line, which is the whole verification surface for that feature.
    //
    // typedef_v deliberately leaves m_Guid at its default (xecs::system::type::details::CreateInfo's
    // own type::guid{__FUNCSIG__} fallback) rather than hand-typing an explicit guid string per
    // system - that automatic, zero-boilerplate identity is worth keeping for the common case. It IS
    // technically less durable than an explicit guid (SystemOrder.config.txt persists it, and a
    // rename or compiler-formatting change could shift it) - but the actual blast radius is small:
    // Load() already treats an unmatched saved guid as "not registered any more" and just skips it
    // (see its own comment), so the worst case is a silently-reset reorder/enable preference, never a
    // crash or corrupted state. Worth switching to an explicit guid on a case-by-case basis for
    // anything where that reset would actually matter (a real gameplay system whose saved order
    // matters for correctness, not a demo).
    struct tick_logger_a : xecs::system::instance
    {
        constexpr static auto typedef_v = xecs::system::type::update{ .m_pName = "Tick Logger A" };

        tick_logger_a(xecs::game_mgr::instance& GameMgr) noexcept : xecs::system::instance(GameMgr) {}

        void OnUpdate(void) noexcept
        {
            std::printf("[System] Tick Logger A\n");
            std::fflush(stdout);
        }
    };

    struct tick_logger_b : xecs::system::instance
    {
        constexpr static auto typedef_v = xecs::system::type::update{ .m_pName = "Tick Logger B" };

        tick_logger_b(xecs::game_mgr::instance& GameMgr) noexcept : xecs::system::instance(GameMgr) {}

        void OnUpdate(void) noexcept
        {
            std::printf("[System] Tick Logger B\n");
            std::fflush(stdout);
        }
    };
}
