import QtQuick
import QtTest
import "../../src/installer"

Item {
    width: 480
    height: 600
    // Test the production page with a deterministic translation context.
    function i18nd(domain, message, first, second) {
        return message.replace("%1", first === undefined ? "" : first)
                      .replace("%2", second === undefined ? "" : second)
    }
    InstallerPage { id: page; width: parent.width }
    TestCase {
        name: "InstallerPage"
        when: windowShown
        function test_phases() {
            for (const phase of ["starting", "downloading", "installing", "complete", "failed"]) {
                page.event = {state: {phase: phase, progress: 50, speed: "10 MiB/s",
                    completed_packages: 1, total_packages: 2}, display: {
                    downloadedSize: "100 MiB", totalDownloadSize: "200 MiB",
                    downloadTimeRemaining: "0:10", errorMessage: "The system update failed."}}
                wait(20)
                compare(page.state.phase, phase)
                verify(page.implicitHeight > 0)
                verify(page.implicitHeight <= page.parent.height)
            }
        }
    }
}
