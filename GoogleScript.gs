         /********************************************
         *                                           *
         *           USER DEFINED SETTINGS           *
         *                                           *
         ********************************************/

// Maximum number of calendar events to download.
// Valid range 1 - 3 (assumes max three collections on one day)
// Default [3]

var maxEvents = 3; 

                             
// The time at which the collection is assumed to have taken place,
// after which the reminder will be automatically dismissed.
// Valid range 0:01am - 11:59pm (0001 - 2359 also acceptable)
// Default [8:00am]

var cutoffTime = "8:00am"   

//=============================================================//
//=============================================================//
                                                                                                               
        /********************************************
        *                                           *
        *      PROGRAM VARIABLES AND FUNCTIONS      *
        *          -- EDIT WITH CAUTION --          *
        *                                           *
        ********************************************/

var dismissed = "dismissed";
var dateRange = 1;

function doGet()
{
  return ContentService.createTextOutput(getEventsJson());
}

function doPost(e)
{
  var clear = e.parameter.clear;
  if (clear === undefined)
  {
    Logger.log("Clear parameter undefined");
    return;
  }
  
  Logger.log("task === clear");
  var cal = CalendarApp.getCalendarsByName('Bins')[0]; 
  
  var now = new Date();
  now.setDate(new Date().getDate());
  Logger.log(now);
  var tomorrow = new Date();
  tomorrow.setDate(new Date().getDate()+1);
  
  var events = cal.getEvents(now, tomorrow);
  Logger.log(events);
  var i;
  for (i = 0; i < events.length; i++)
  {
    events[i].setTag(dismissed, "TRUE");
    Logger.log("Dismissed: " + events[i].getTag(dismissed));
  }
  
}

function getEventsJson()
{
  var cal = CalendarApp.getCalendarsByName('Bins')[0]; 
  var now = new Date();
  now.setDate(new Date().getDate());
  var future = new Date();
  future.setDate(new Date().getDate() + dateRange);

  // If the cut-off time has passed, skip to tomorrow
  Logger.log(parseTime(cutoffTime));
  if (now > parseTime(cutoffTime))
  {
    now.setDate(now.getDate() + 1);
    now.setHours(00);
    now.setMinutes(00);
    }
    
  var events = cal.getEvents(now, future)
  if (events.length > 0)
  {
    var event = [];
    var i;
    var j = events.length;
    if (events.length >= maxEvents)
      j = maxEvents;
    for (i = 0; i < j; i++)
    {
      if (events[i].getTag(dismissed) !== "TRUE")
        event.push({"title": events[i].getTitle(), "color": events[i].getColor()});
    }
    var payload = JSON.stringify(event);
    Logger.log(payload);
    return payload;
  }
  
    var event = [];
    var payload = JSON.stringify(event);
    Logger.log(payload);
    return payload;
}

function parseTime(t) {
   var d = new Date();
   d.setDate(new Date().getDate());
   var time = t.match( /(\d+)(?::(\d\d))?\s*(p?)/ );
   d.setHours(parseInt( time[1]) + (time[3] ? 12 : 0) );
   d.setMinutes(parseInt( time[2]) || 0 );
   d.setSeconds(0);
   return d;
}

        /********************************************
        *                                           *
        *                 DEBUGGING                 *
        *         -- CALL FROM RUN MENU --          *
        *                                           *
        ********************************************/

function resetEventTags()
{
  var Cal = CalendarApp.getCalendarsByName('Bins')[0]; 
  
  var now = new Date();
  now.setDate(new Date().getDate());
  var future = new Date();
  future.setDate(new Date().getDate()+31);
  
  var events = Cal.getEvents(now, future);
  var i;
  for (i = 0; i < events.length; i++)
  {
    events[i].setTag(dismissed, "FALSE");
    Logger.log("Dismissed: " + events[i].getTag(dismissed));
  }
}

function test()
{
  var e = {
  "parameter": {
    "clear": ""
    }
    };
  
  doPost(e);
}


