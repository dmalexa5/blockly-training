import * as Blockly from 'blockly';
import 'blockly/blocks';
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

runButton.disabled = true;
stopButton.disabled = true;

type ModuleConfig = {
  id: string;
  type: string;
  label: string;
};

const defaultModules: ModuleConfig[] = [
  { id: 'button', type: 'button', label: 'Button' },
  { id: 'rebounder', type: 'rebounder', label: 'Rebounder' },
];

async function loadModules(): Promise<ModuleConfig[]> {
  try {
    const response = await fetch('/api/modules');
    if (!response.ok) {
      return defaultModules;
    }
    const payload = await response.json();
    if (!Array.isArray(payload.modules)) {
      return defaultModules;
    }
    return payload.modules;
  } catch {
    return defaultModules;
  }
}

function moduleOptions(modules: ModuleConfig[], type: string): Blockly.MenuOption[] {
  const options = modules
    .filter((module) => module.type === type)
    .map((module) => [module.label, module.id] as Blockly.MenuOption);
  if (options.length > 0) {
    return options;
  }
  return defaultModules
    .filter((module) => module.type === type)
    .map((module) => [module.label, module.id] as Blockly.MenuOption);
}

const toolbox = {
  kind: 'flyoutToolbox',
  contents: [
    {
      kind: 'block',
      type: 'controls_repeat_ext',
      inputs: {
        TIMES: {
          shadow: {
            type: 'math_number',
            fields: {
              NUM: 3,
            },
          },
        },
      },
    },
    { kind: 'block', type: 'rbdr_button' },
    { kind: 'block', type: 'rbdr_rebounder' },
    { kind: 'block', type: 'rbdr_wait' },
  ],
};

function defineBlocks(modules: ModuleConfig[]): void {
  Blockly.Blocks['rbdr_button'] = {
    init() {
      this.appendDummyInput()
        .appendField('button')
        .appendField(new Blockly.FieldDropdown(moduleOptions(modules, 'button')), 'MODULE');
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setColour(196);
      this.setTooltip('Activate the button module and wait for a press event.');
    },
  };

  Blockly.Blocks['rbdr_rebounder'] = {
    init() {
      this.appendDummyInput()
        .appendField('rebounder')
        .appendField(new Blockly.FieldDropdown(moduleOptions(modules, 'rebounder')), 'MODULE');
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setColour(24);
      this.setTooltip('Activate the rebounder module and wait for a trigger event.');
    },
  };

  Blockly.Blocks['rbdr_wait'] = {
    init() {
      this.appendDummyInput()
        .appendField('wait')
        .appendField(new Blockly.FieldNumber(1, 1, 10, 1), 'SECONDS')
        .appendField('seconds');
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setColour(60);
      this.setTooltip('Wait for 1 to 10 seconds before continuing.');
    },
  };
}

pythonGenerator.forBlock['rbdr_button'] = (block) =>
  `await rbdr.activate_and_wait("${block.getFieldValue('MODULE') ?? 'button'}")\n`;
pythonGenerator.forBlock['rbdr_rebounder'] = (block) =>
  `await rbdr.activate_and_wait("${block.getFieldValue('MODULE') ?? 'rebounder'}")\n`;
pythonGenerator.forBlock['rbdr_wait'] = (block) => `await rbdr.wait(${block.getFieldValue('SECONDS')})\n`;

let workspace: Blockly.WorkspaceSvg;

function generateCode(): string {
  const code = pythonGenerator.workspaceToCode(workspace);
  codeOutput.textContent = code || '# Add button, rebounder, or wait blocks, then run.\n';
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

async function main(): Promise<void> {
  defineBlocks(await loadModules());

  workspace = Blockly.inject(blocklyArea, {
    toolbox,
    trashcan: true,
  });

  workspace.addChangeListener(() => {
    generateCode();
  });

  generateCode();
  setRunning(false);
  connectEvents();
}

void main();
