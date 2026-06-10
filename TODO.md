Confirming implementation status of the codebase against AGENTS.md and wiki docs:
- **AGENTS.md**: Mentions "SRT Parser (4u) 📝 Planned" but the source code (`src/srt_parser.c`) is present and registered as a plugin. Also, the build output includes building `srt_parser.c`. This needs to be updated.
- **wiki/implementation-plan.md**: Mentions "4u planned" in the quick status and table, but `srt_parser.c` is done. Also says "📝 4q, 4r, 4u planned" which needs to be "📝 4q, 4r planned".
- **wiki/phase-elements.md**: Mentions "📝 4q, 4r, 4u" in the title header but then says "4u - SRT Subtitle Parser (✅ done)" below.
- **tests/test_net_sink.c**: Build is failing because of implicit declarations `usleep` and missing arguments to `zst_buffer_create`.

Findings:
The codebase has `srt_parser.c` implemented, but `AGENTS.md` and `wiki/implementation-plan.md` still say it is "Planned". `wiki/phase-elements.md` has conflicting information in the header vs the section. Also, tests are currently failing to build (`test_net_sink.c`).
