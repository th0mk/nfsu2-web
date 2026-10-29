//! The online host's tests, most of them driving the single-threaded core with manual clocks (world.rs): the
//! services (lobby.rs), claimed names (claims.rs), the virtual host (vhost.rs), the Online audit's cases (audit.rs), edge cases (edge.rs) and
//! random sessions of the whole host (session.rs); the host on a thread of its own (thread.rs); a rough measure of
//! its cost (load.rs, ignored by default).

mod audit;
mod claims;
mod edge;
mod load;
mod lobby;
mod session;
mod thread;
mod vhost;
mod world;
