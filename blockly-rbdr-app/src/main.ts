import * as Blockly from 'blockly';
import { pythonGenerator } from 'blockly/python';
import './style.css';

function getElement<T extends Element>(selector: string): T {
  const element = document.querySelector<T>(selector);
  if (!element) {
    throw new Error(`Missing app element: ${selector}`);
  }
  return element;
}

const blocklyArea = getElement<HTMLDivElement>('#blocklyArea');
const runButton = getElement<HTMLButtonElement>('#runButton');
const stopButton = getElement<HTMLButtonElement>('#stopButton');
const statusNode = getElement<HTMLElement>('#status');
const codeOutput = getElement<HTMLPreElement>('#codeOutput');
const logOutput = getElement<HTMLOListElement>('#logOutput');

Blockly.Blocks['rbdr_button'] = {
  init() {
    this.appendDummyInput().appendField('button');
    this.setPreviousStatement(true, null);
    this.setNextStatement(true, null);
    this.setColour(196);
    this.setTooltip('Activate the button module and wait for a press event.');
  },
};

Blockly.Blocks['rbdr_rebounder'] = {
  init() {
    this.appendDummyInput().appendField('rebounder');
    this.setPreviousStatement(true, null);
    this.setNextStatement(true, null);
    this.setColour(24);
    this.setTooltip('Activate the rebounder module and wait for a trigger event.');
  },
};

pythonGenerator.forBlock['rbdr_button'] = () => 'await rbdr.activate_and_wait("button")\n';
pythonGenerator.forBlock['rbdr_rebounder'] = () => 'await rbdr.activate_and_wait("rebounder")\n';

const toolbox = {
  kind: 'flyoutToolbox',
  contents: [
    { kind: 'block', type: 'rbdr_button' },
    { kind: 'block', type: 'rbdr_rebounder' },
  ],
};

const workspace = Blockly.inject(blocklyArea, {
  toolbox,
  trashcan: true,
});

function generateCode(): string {
  const code = pythonGenerator.workspaceToCode(workspace);
  codeOutput.textContent = code || '# Add button or rebounder blocks, then run.\n';
  return code;
}

function appendLog(message: string): void {
  const item = document.createElement('li');
  item.textContent = message;
  logOutput.append(item);
  logOutput.scrollTop = logOutput.scrollHeight;
}

function setRunning(running: boolean): void {
  runButton.disabled = running;
  stopButton.disabled = !running;
}

workspace.addChangeListener(() => {
  generateCode();
});

function connectEvents(): void {
  const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(`${scheme}://${window.location.host}/api/ws`);

  socket.addEventListener('message', (event) => {
    const payload = JSON.parse(event.data);
    if (payload.type === 'status') {
      statusNode.textContent = payload.status;
      setRunning(payload.status === 'running');
    }
    if (payload.type === 'log') {
      appendLog(payload.message);
    }
  });

  socket.addEventListener('close', () => {
    statusNode.textContent = 'disconnected';
    setRunning(false);
    window.setTimeout(connectEvents, 1000);
  });
}

runButton.addEventListener('click', async () => {
  const code = generateCode();
  logOutput.replaceChildren();

  const response = await fetch('/api/run', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ code }),
  });

  if (!response.ok) {
    const payload = await response.json().catch(() => ({ detail: 'Run failed' }));
    appendLog(payload.detail);
  }
});

stopButton.addEventListener('click', async () => {
  await fetch('/api/stop', { method: 'POST' });
});

generateCode();
setRunning(false);
connectEvents();
