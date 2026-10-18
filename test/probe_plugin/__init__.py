"""API test plugin: exercises the 'structbridge' Python module end-to-end."""

from endstone.plugin import Plugin


class ProbePlugin(Plugin):
    def on_enable(self):
        log = self.logger
        try:
            import structbridge
        except ImportError as e:
            log.error(f"FAIL: cannot import structbridge: {e}")
            return
        self._tries = 0
        self.server.scheduler.run_task(self, self._wait_probe, delay=20)

    def _wait_probe(self):
        import structbridge
        if not structbridge.is_available():
            self._tries += 1
            if self._tries >= 30:
                self.logger.error("probe never became available; giving up")
                return
            self.server.scheduler.run_task(self, self._wait_probe, delay=20)
            return
        self._run_tests()

    def _run_tests(self):
        log = self.logger
        import structbridge

        log.info(f"api_version={structbridge.api_version()} "
                 f"plugin_version={structbridge.plugin_version()} "
                 f"is_available={structbridge.is_available()}")
        log.info(f"stage_message(0)={structbridge.stage_message(0)!r}")
        log.info(f"status={structbridge.status()}")

        # 1) list
        items = structbridge.list_structures()
        log.info(f"list_structures -> {items}")

        # 2) import by library name (123 was left in the library by run37)
        r = structbridge.import_structure("123")
        log.info(f"import_structure('123') -> {r}")

        # 3) dump an already-saved structure
        r = structbridge.dump_structure("123")
        log.info(f"dump_structure('123') -> {r}")

        # 4) async export with callback (region around the test platform)
        def on_done(stage, message):
            log.info(f"[export callback] stage={stage} ({structbridge.stage_message(stage)}) message={message!r}")
            after = structbridge.list_structures()
            log.info(f"list_structures after export -> {after}")
            d = structbridge.delete_structure("apitest")
            log.info(f"delete_structure('apitest') -> {d}")
            final = structbridge.list_structures()
            log.info(f"final list -> {final}")

        job = structbridge.export_region("apitest", 8, 63, 8, 12, 65, 12, on_done=on_done)
        log.info(f"export_region -> job id {job}")

        # 5) rejection paths
        bad = structbridge.export_region("bad name!", 0, 63, 0, 1, 63, 1)
        log.info(f"export_region(bad name) -> {bad} (expect 0)")
        nf = structbridge.import_structure("no_such_structure_file")
        log.info(f"import_structure(missing) -> {nf}")
