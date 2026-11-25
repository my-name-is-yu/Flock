export const actionLabels = {
  'bot.view': 'View Bot conversations',
  'bot.talk': 'Talk to Bots',
  'bot.decide': 'Respond to Bot requests',
  'bot.settings': 'Change Bot settings',
  'bot.create': 'Create Bots',
  'bot.rename': 'Rename Bots',
  'rules.manage': 'Manage permission rules',
  'repository.read': 'Read repositories',
  'repository.write': 'Write to repositories',
  'repository.pr': 'Open or update pull requests',
  'repository.review': 'Review pull requests',
  'repository.issues': 'Manage issues',
  'secret.use': 'Use secrets',
  'session.read': 'Read T3 Sessions',
  'mcp.discover': 'List MCP tools',
  'mcp.invoke': 'Call MCP tools',
  'mcp.read_resource': 'Read MCP resources',
  'environment.inspect': 'View T3 environments',
  'environment.configure': 'Configure T3 environments',
  'environment.launch': 'Start T3 Sessions',
  'shared_memory.read': 'Read shared memory',
  'shared_memory.write': 'Write to shared memory',
  'memory.write': 'Edit Bot memory',
  'workspace.read': 'Read workspace files',
  'workspace.write': 'Write workspace files',
  'workspace.exec': 'Run workspace commands',
  'web.search': 'Search the web',
  'web.fetch': 'Open web pages',
  'skill.list': 'List skills',
  'skill.read': 'Read skills',
  'attachment.inspect': 'View attachments',
  'channel.use': 'Use channels',
  'channel.read': 'Read channel messages',
  'channel.post': 'Post channel messages',
  't3.send': 'Send messages to T3',
  'automation.change': 'Change automations',
  'email.send': 'Send email',
  'data.destructive_write': 'Make destructive changes',
};
export const actionLabel = (action) => Object.hasOwn(actionLabels, action) ? actionLabels[action] : action;

const approvalFieldLabels = {
  prNumber: 'Pull request', issueNumber: 'Issue', repoId: 'Repository',
  environmentId: 'Environment', projectId: 'Project', url: 'URL',
};
const blank = (value) => value == null || value === '' ||
  (typeof value === 'object' && Object.values(value).every(blank));

// Flock writes a delegated instruction as the request, then "Task intent:" and "Completion criteria:"
// sections for the Session. Show the request once and the criteria as "Done when".
function instructionBody(text) {
  return text.replace(/\n+\[Flock action ID: flock-[^\]\n]+\]\s*$/, '');
}

function instructionLines(text) {
  const body = instructionBody(text);
  const sections = body.split(/\n\n(Task intent|Completion criteria):\n/);
  if (sections.length !== 5 || sections[1] !== 'Task intent' || sections[3] !== 'Completion criteria')
    return [body.trim()].filter(Boolean);
  const [request, , goal, , doneWhen] = sections.map((section) => section.trim());
  const lines = [request];
  if (goal && goal !== request) lines.push(`Goal: ${goal}`);
  if (doneWhen) lines.push(`Done when: ${doneWhen}`);
  return lines.filter(Boolean);
}

export function approvalSummary(preview, { action, target } = {}) {
  if (typeof preview !== 'string' || !preview.trim()) return '';
  let value;
  try { value = JSON.parse(preview); } catch { return preview.trim(); }
  if (typeof value === 'string') return value.trim();
  const [operation, fields] = Array.isArray(value) && value.length === 2 && typeof value[0] === 'string'
    ? value : [null, value];
  if (!fields || typeof fields !== 'object' || Array.isArray(fields)) return preview.trim();
  // An environment approval can also gate work in an existing Session. Its target and operation still matter.
  const startsNewSession = action === 'environment.launch' && operation === null && fields.tool === 'delegate';
  const lines = operation ? [operation.replaceAll('_', ' ')] : [];
  if (startsNewSession && typeof fields.instruction === 'string') lines.push(...instructionLines(fields.instruction));
  for (const [key, value] of Object.entries(fields)) {
    const instruction = key === 'instruction' && typeof value === 'string';
    if (startsNewSession && (instruction || key === 'tool' ||
      (key === 'environmentId' && value === target) || (['run', 'questionId'].includes(key) && blank(value)))) continue;
    const label = Object.hasOwn(approvalFieldLabels, key) ? approvalFieldLabels[key] : key.replace(/([a-z])([A-Z])/g, '$1 $2');
    const text = typeof value === 'string' ? value : JSON.stringify(value);
    const heading = label.charAt(0).toUpperCase() + label.slice(1);
    lines.push(`${heading}: ${instruction ? instructionBody(text) : text}`);
  }
  return lines.join('\n');
}
