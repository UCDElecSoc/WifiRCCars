# To run server, assuming you have already cloned the "makerlab-remote-control" repo
- Install git, python (v3.13.12). nodejs (LTS v24.14.1)

cd `~\GitHub\makerlab-remote-control>`

## If an existing .venv folder exists, and it's your first time:
- Ensure your not in venv currently, and if so, `deactivate`
- `Remove-Item -Recurse -Force .venv`
- Then `py -3.13 -m venv .venv` to create the environment

## Start the enviroment
- Activate venv `.\.venv\Scripts\Activate`
- If first time, `pip install -r server/requirements.txt`
- If first time, `Ctrl+Shift+P`, `Python: Select Interpreter`, and choose correct venv (you need Python extension for VSCode)


*For the following, please use the correct server IP. In testing, it will be `192.168.50.51`. In deployment, it will be `192.168.50.50`

## Run Asynchronous Server Gateway Interface
- Open a new PowerShell terminal.
- Run `uvicorn app.main:app --app-dir server --reload --host 0.0.0.0 --port 8000`
*This starts the back-end, with REST API at `http://192.168.50.50:8000/api/...` and websocket at `ws://192.168.50.51:8000/ws`. `0.0.0.0` allows connectoins from other devices*

## Run front end (locally)
- Open new Powershell terminal
- `cd frontend`
- `npm install`
- `npm run dev`

## Run front end (for other devices to access)
- `cd frontend`
- `$env:VITE_WS_URL="ws://192.168.50.51:8000/ws"`; defines where frontend connects; Used for WebSocket connection; Browser → server real-time messages
- `$env:VITE_API_BASE_URL="http://192.168.50.51:8000"`; Use for REST API calls e.g. /api/targets
- `npm run dev -- --host`; Run Vite dev server and passes host to Vite to allow access over LAN