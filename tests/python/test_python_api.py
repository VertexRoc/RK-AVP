from pathlib import Path
import gc
import json
import threading

import pytest

import rkavp


ROOT = Path(__file__).resolve().parents[2]


def write_io_graph(tmp_path):
    path = tmp_path / "io.yaml"
    path.write_text(
        """version: 1
graph:
  name: python_io
  inputs:
    input: pass.in
  outputs:
    output: pass.out
  nodes:
    - id: pass
      type: Passthrough
""",
        encoding="ascii",
    )
    return path


def write_source_graph(tmp_path):
    path = tmp_path / "sources.yaml"
    path.write_text(
        """version: 2
graph:
  name: python_sources
  inputs:
    input: {to: pass.in, queue: {capacity: 16, policy: block}}
  outputs:
    output: pass.out
  source_slots:
    camera: input
  nodes:
    - id: pass
      type: Passthrough
""",
        encoding="ascii",
    )
    return path


def test_validate_builtin_graph():
    assert rkavp.validate(str(ROOT / "graphs" / "passthrough.yaml"))


def test_graph_lifecycle():
    graph = rkavp.Graph(str(ROOT / "graphs" / "passthrough.yaml"))
    assert "graph=passthrough" in graph.inspection
    graph.start()
    assert graph.running
    graph.stop()
    assert not graph.running


def test_invalid_graph_raises_runtime_error(tmp_path):
    path = tmp_path / "invalid.yaml"
    path.write_text("version: 3\ngraph: {nodes: []}\n", encoding="ascii")
    with pytest.raises(RuntimeError, match="unsupported graph version"):
        rkavp.validate(str(path))


def test_structured_packet_callback_and_wait(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    received = []
    ready = threading.Event()
    graph.observe_output("output", lambda packet: (received.append(packet), ready.set()))
    graph.start()
    graph.add_packet("input", {"text": "hello", "scores": [1, 2.5]}, 1234)
    graph.wait_until_idle(1000)
    assert ready.wait(1)
    assert received[0]["timestamp_us"] == 1234
    assert received[0]["value"] == {"text": "hello", "scores": [1, 2.5]}
    graph.close_input("input")
    graph.wait_until_done(1000)
    graph.stop()


def test_callback_exception_is_isolated(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    called = threading.Event()

    def broken_callback(_packet):
        called.set()
        raise ValueError("business callback failed")

    graph.observe_output("output", broken_callback, queue_capacity=1)
    graph.start()
    graph.add_packet("input", "value", 1)
    graph.wait_until_idle(1000)
    assert called.wait(1)
    for _ in range(100):
        if graph.callback_errors:
            break
        threading.Event().wait(0.001)
    assert graph.callback_errors == 1
    assert graph.running
    graph.stop()


def test_graph_release_stops_resources(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    graph.observe_output("output", lambda _packet: None)
    graph.start()
    del graph
    gc.collect()


@pytest.mark.parametrize(
    "value",
    [None, True, 42, 3.5, "text", [1, False, None], {"nested": {"items": [1, 2]}}],
)
def test_config_value_round_trip(tmp_path, value):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    received = []
    ready = threading.Event()
    graph.observe_output("output", lambda packet: (received.append(packet), ready.set()))
    graph.start()
    graph.add_packet("input", value, 7)
    graph.wait_until_idle(1000)
    assert ready.wait(1)
    assert received == [{"timestamp_us": 7, "event": 0, "value": value}]
    graph.stop()


def test_multiple_output_observers_receive_same_packet(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    received = [[], []]
    ready = [threading.Event(), threading.Event()]
    for index in range(2):
        graph.observe_output(
            "output",
            lambda packet, index=index: (received[index].append(packet), ready[index].set()),
        )
    graph.start()
    graph.add_packet("input", "fanout", 10)
    graph.wait_until_idle(1000)
    assert all(event.wait(1) for event in ready)
    assert received[0] == received[1]
    graph.stop()


def test_slow_python_observer_uses_bounded_queue(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    entered = threading.Event()
    release = threading.Event()

    def slow_callback(_packet):
        entered.set()
        release.wait(1)

    graph.observe_output("output", slow_callback, queue_capacity=1)
    graph.start()
    graph.add_packet("input", 0, 0)
    assert entered.wait(1)
    for index in range(1, 20):
        graph.add_packet("input", index, index)
    graph.wait_until_idle(1000)
    assert graph.observer_dropped > 0
    assert graph.running
    release.set()
    graph.stop()


def test_side_packet_is_immutable_and_must_precede_start(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    graph.set_side_packet("session", {"name": "python"})
    with pytest.raises(RuntimeError, match="immutable"):
        graph.set_side_packet("session", "replacement")
    graph.start()
    with pytest.raises(RuntimeError, match="before Start"):
        graph.set_side_packet("late", True)
    graph.stop()


def test_timestamp_bound_is_monotonic(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    graph.start()
    graph.set_input_timestamp_bound("input", 20)
    with pytest.raises(RuntimeError, match="cannot move backwards"):
        graph.set_input_timestamp_bound("input", 19)
    graph.stop()


def test_cancel_unblocks_wait_and_reports_state(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    graph.start()
    graph.cancel()
    assert graph.cancelled
    with pytest.raises(RuntimeError, match="cancelled"):
        graph.wait_until_done(1000)
    graph.stop()


def test_repeated_start_stop_and_trace_export(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    for iteration in range(2):
        graph.start()
        graph.add_packet("input", iteration, iteration)
        graph.wait_until_idle(1000)
        graph.stop()
    trace = json.loads(graph.trace_json)
    assert trace["traceEvents"]
    assert "node.pass.processed" in graph.metrics


def test_error_callback_must_be_registered_before_start(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    graph.set_error_callback(lambda _code, _message: None)
    graph.start()
    with pytest.raises(RuntimeError, match="before Start"):
        graph.set_error_callback(lambda _code, _message: None)
    graph.stop()


def test_invalid_streams_and_observer_capacity_are_actionable(tmp_path):
    graph = rkavp.Graph(str(write_io_graph(tmp_path)))
    with pytest.raises(ValueError, match="positive"):
        graph.observe_output("output", lambda _packet: None, queue_capacity=0)
    with pytest.raises(RuntimeError, match="output stream not found"):
        graph.observe_output("missing", lambda _packet: None)
    graph.start()
    with pytest.raises(RuntimeError, match="input stream not found"):
        graph.add_packet("missing", 1, 1)
    graph.stop()


def test_dynamic_sources_and_runtime_info_are_control_plane_only(tmp_path):
    graph = rkavp.Graph(str(write_source_graph(tmp_path)))
    received = []
    ready = threading.Event()
    graph.observe_output("output", lambda packet: (received.append(packet), ready.set()))
    graph.start()
    graph.add_source("camera", "front", {"uri": "/dev/video0"})
    assert graph.list_sources()[0]["source_id"] == "front"
    graph.add_source_packet("front", {"control": "payload"}, 1)
    graph.wait_until_idle(1000)
    assert ready.wait(1)
    assert graph.source_health("front")["state"] == "streaming"
    runtime = graph.runtime_info
    assert runtime["running"] is True
    assert runtime["executors"]
    assert any(stream["name"] == "input" for stream in runtime["streams"])
    graph.restart_source("front")
    assert graph.source_health("front")["state"] == "reconnecting"
    graph.remove_source("front", 1000)
    assert graph.list_sources() == []
    graph.stop()
