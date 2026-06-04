import os
import json
from flask import Flask, jsonify
import dash
from dash import html, dcc
from dash.dependencies import Input, Output
import plotly.graph_objs as go
import numpy as np

# Specify the absolute path to your results file.
RESULTS_FILE = "/tmp/results.json"

# Create a Flask server instance.
server = Flask(__name__)


@server.route('/results')
def get_results():
    if os.path.exists(RESULTS_FILE):
        with open(RESULTS_FILE, "r") as f:
            data = json.load(f)
        return jsonify(data)
    else:
        return jsonify([])


# Create a Dash app embedded in the Flask server.
app = dash.Dash(__name__, server=server, routes_pathname_prefix='/dashboard/')

# Global variable to keep history of challenges (each stored as in results.json)
challenge_history = []  # Each element is a dict as produced in results.json

app.layout = html.Div([
    html.H1("HFT Challenge Dashboard"),
    dcc.Interval(id='interval-update', interval=5000, n_intervals=0),  # update every 5 sec
    html.H2("Aggregated Statistics"),
    html.Div(id='aggregated-stats'),
    dcc.Graph(id='overall-latency-graph'),
    dcc.Graph(id='client-latency-chart'),
    dcc.Graph(id='victory-chart'),
    dcc.Graph(id='latency-histogram'),
    html.H2("Raw Data"),
    html.Pre(id='raw-data', style={'overflowY': 'scroll', 'height': '300px'})
])


@app.callback(
    [Output('aggregated-stats', 'children'),
     Output('overall-latency-graph', 'figure'),
     Output('client-latency-chart', 'figure'),
     Output('victory-chart', 'figure'),
     Output('latency-histogram', 'figure'),
     Output('raw-data', 'children')],
    [Input('interval-update', 'n_intervals')]
)
def update_dashboard(n_intervals):
    # Read the entire history from RESULTS_FILE.
    if os.path.exists(RESULTS_FILE):
        try:
            with open(RESULTS_FILE, "r") as f:
                challenges = json.load(f)
        except Exception as e:
            print("Error reading results file:", e)
            challenges = []
    else:
        challenges = []

    # Dictionaries for aggregated stats.
    wins = {}  # wins count per client (from challenge["winner"])
    participation = {}  # number of challenges in which a client appears
    latencies_by_client = {}  # client -> list of latencies (all challenges)
    overall_challenge_avg = []  # list of (challenge_id, overall avg latency)
    client_challenge_data = {}  # client -> list of (challenge_id, latency)

    for ch in challenges:
        cid = ch.get("challenge_id")
        players = ch.get("players", [])
        if players:
            avg_ch = sum(player.get("latency_ms", 0) for player in players) / len(players)
        else:
            avg_ch = 0
        overall_challenge_avg.append((cid, avg_ch))

        # Process each player's data in the challenge.
        for player in players:
            name = player.get("name")
            latency = player.get("latency_ms")
            if name and latency is not None:
                participation[name] = participation.get(name, 0) + 1
                latencies_by_client.setdefault(name, []).append(latency)
                client_challenge_data.setdefault(name, []).append((cid, latency))

        # Count victories from the declared winner.
        winner = ch.get("winner", "")
        if winner:
            wins[winner] = wins.get(winner, 0) + 1

    # Compute average latency per client.
    avg_latency_client = {
        name: sum(lat_list) / len(lat_list)
        for name, lat_list in latencies_by_client.items() if lat_list
    }
    # Compute win rate in percentage.
    win_rate = {
        name: (wins.get(name, 0) / participation[name] * 100) if participation[name] > 0 else 0
        for name in participation
    }

    # Build an aggregated statistics table.
    all_clients = sorted(set(participation.keys()) | set(wins.keys()))
    stats_table = html.Table(
        [html.Tr([html.Th("Client"), html.Th("Participation"), html.Th("Victories"),
                  html.Th("Win Rate (%)"), html.Th("Avg Latency (ms)")])] +
        [html.Tr([html.Td(client),
                  html.Td(participation.get(client, 0)),
                  html.Td(wins.get(client, 0)),
                  html.Td(round(win_rate.get(client, 0), 2)),
                  html.Td(round(avg_latency_client.get(client, 0), 2))])
         for client in all_clients]
    )

    # Line Chart: Overall average latency per challenge.
    overall_trace = go.Scatter(
        x=[item[0] for item in overall_challenge_avg],
        y=[item[1] for item in overall_challenge_avg],
        mode='lines+markers',
        name='Overall Avg Latency'
    )
    overall_latency_fig = go.Figure(data=[overall_trace])
    overall_latency_fig.update_layout(title='Overall Average Latency per Challenge',
                                      xaxis_title='Challenge ID',
                                      yaxis_title='Latency (ms)')

    # Multi-line chart: Each client's latency over challenges.
    client_traces = []
    for client, data in client_challenge_data.items():
        data_sorted = sorted(data, key=lambda x: x[0])
        client_trace = go.Scatter(
            x=[point[0] for point in data_sorted],
            y=[point[1] for point in data_sorted],
            mode='lines+markers',
            name=client
        )
        client_traces.append(client_trace)
    client_latency_fig = go.Figure(data=client_traces)
    client_latency_fig.update_layout(title='Client Latency per Challenge',
                                     xaxis_title='Challenge ID',
                                     yaxis_title='Latency (ms)')

    # Bar Chart: Total victories per client.
    victory_trace = go.Bar(
        x=list(wins.keys()),
        y=[wins[name] for name in wins],
        name='Victories'
    )
    victory_fig = go.Figure(data=[victory_trace])
    victory_fig.update_layout(title='Total Victories per Client',
                              xaxis_title='Client',
                              yaxis_title='Victories')

    # Histogram: Distribution of all recorded latencies.
    all_latency_values = []
    for lat_list in latencies_by_client.values():
        all_latency_values.extend(lat_list)
    histogram_fig = go.Figure(data=[go.Histogram(x=all_latency_values, nbinsx=20)])
    histogram_fig.update_layout(title='Latency Distribution',
                                xaxis_title='Latency (ms)',
                                yaxis_title='Count')

    raw_data = json.dumps(challenges, indent=2)

    return stats_table, overall_latency_fig, client_latency_fig, victory_fig, histogram_fig, raw_data


if __name__ == '__main__':
    app.run(debug=True, port=5000)
