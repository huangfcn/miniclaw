# Windows Task Scheduler Skill

This skill allows you to create, manage, and run scheduled tasks using Windows Task Scheduler from the command line.

## Tools

Use the `exec` tool to run Windows `schtasks` commands.

## Create a One-Time Task

```bash
schtasks /Create /TN "MyTaskName" /TR "C:\path\to\script.bat" /SC ONCE /ST HH:MM
```

## Create a Recurring Task (Daily)

```bash
schtasks /Create /TN "MyDailyTask" /TR "C:\path\to\script.bat" /SC DAILY /ST HH:MM
```

## Create a Recurring Task (Weekly)

```bash
schtasks /Create /TN "MyWeeklyTask" /TR "C:\path\to\script.bat" /SC WEEKLY /D MON /ST HH:MM
```

## Create a Recurring Task (Hourly)

```bash
schtasks /Create /TN "MyHourlyTask" /TR "C:\path\to\script.bat" /SC HOURLY /MO 1
```

## List All Scheduled Tasks

```bash
schtasks /Query /FO LIST
```

## Run a Task Immediately

```bash
schtasks /Run /TN "MyTaskName"
```

## Delete a Task

```bash
schtasks /Delete /TN "MyTaskName" /F
```

## Modify a Task

```bash
schtasks /Change /TN "MyTaskName" /TR "C:\new\path\to\script.bat"
```

## Tips

- `/TN` = Task Name
- `/TR` = Task Run (command/script to execute)
- `/SC` = Schedule (ONCE, DAILY, WEEKLY, MONTHLY, HOURLY, etc.)
- `/ST` = Start Time (HH:MM 24-hour format)
- `/D` = Days (MON, TUE, WED, THU, FRI, SAT, SUN)

Run as Administrator for some operations!