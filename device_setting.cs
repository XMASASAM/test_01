using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using QBS_WinUI.Components.Utility;
using QBS_WinUI.MediaCapture.Class;
using QBS_WinUI.MediaCapture.Test.Model;
using R3;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.System;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace QBS_WinUI.MediaCapture.Dialogs
{



	public class DeviceSettingModel:R3Extention.R3PropertyBase
	{
		public class JsonData
		{
			public string? VideoName {  get; set; }
			public string? VideoPath {  get; set; }
			public string? VideoPinName { get; set; }
			public int? Width { get; set; }
			public int? Height { get; set; }
			public Rotate? VideoRotate { get; set; }
			public string? AudioName { get; set; }
			public string? AudioPath { get; set; }
			public string? AudioPinName { get; set; }
			public int? AudioSampleRate { get; set; }
			public int? AudioType { get; set; }
		}


		IDeviceReader DeviceReader = new Models.QBSDeviceReaderModel();
		//DispatcherQueue DispatcherQueue = DispatcherQueue.GetForCurrentThread();
		public ReactiveProperty<List<NameInfo>?> VideoDeviceList = new (null);
		public ReactiveProperty<List<NameInfo>?> AudioDeviceList = new (null);
		public ReactiveProperty<List<VideoInfo>?> VideoDetailsList = new(null);

		public ReactiveProperty<List<int>?> AudioSampleRateList = new(null);
		public ReactiveProperty<List<int>?> AudioTypeList = new(null);


		public ReactiveProperty<List<AudioInfo>?> AudioDetailsList = new(null);

		Task<List<NameInfo>> LoadVideoTask;
		Task<List<NameInfo>> LoadAudioTask;

		//設定する値.
		public ReactiveProperty<NameInfo> VideoDevice = new();
		public ReactiveProperty<VideoInfo> VideoDitals = new();
		public ReactiveProperty<Rotate> VideoRotate = new();
		//
		public ReactiveProperty<NameInfo> AudioDevice = new();
		public ReactiveProperty<int> AudioSampleRate = new();
		public ReactiveProperty<int> AudioType = new();


		public ReactiveProperty<VideoInfo> VideoDetail = new();

		public Dictionary<NameInfo, Task<List<NameInfo>>> AudioListMap = new();
		public Dictionary<NameInfo, Task<List<PinInfo>>> VideoDetailsMap = new();
		public Dictionary<NameInfo, Task<List<PinInfo>>> AudioDetailsMap = new();

		public ReadOnlyReactiveProperty<bool> IsLoadingVideoDevice;
		public ReadOnlyReactiveProperty<bool> IsLoadingAudioDevice;
		public ReadOnlyReactiveProperty<bool> IsLoadingVideoReso;
		public ReadOnlyReactiveProperty<bool> IsLoadingAudioDetails;

		int video_details_mutex_count = 0;
		int audio_details_mutex_count = 0;

		JsonData? PrevData;

		public DeviceSettingModel(string? json_string=null)
		{

			VideoDevice.Subscribe((name_info) => {
				video_details_mutex_count++;
				VideoResoChange(name_info, video_details_mutex_count);
				AudioListChenge(name_info, video_details_mutex_count);
			}).AddTo(Disposables);


			AudioDevice.Subscribe(async (name_info) =>
			{
				audio_details_mutex_count++;
				AudioSampleRateList.Value = null;
				await LoadItems(name_info, audio_details_mutex_count, () => audio_details_mutex_count,AudioDetailsList, AudioDetailsMap,
					async (prev) =>
					{
						return await DeviceReader.LoadPinInfoList(MediaType.Audio, name_info.Name, name_info.Duplicate);
					}, x =>
					{
						if (x.FirstOrDefault(x => x.AudioInfoList.Count != 0) is PinInfo pin)
						{

							AudioSampleRateList.Value = pin.AudioInfoList.Select(x=>x.Samples).Distinct().ToList();

							return pin.AudioInfoList;
						}
						return new List<AudioInfo>();

					}
				);

			});

			AudioSampleRate.Subscribe((sample) => {
				AudioTypeList.Value = null;
				if (AudioDetailsList.Value is null) return;
				AudioTypeList.Value = AudioDetailsList.Value
					.Where(x => x.Samples == sample)
					.Select(x=>x.Channels)
					.Distinct().ToList();
			});


			IsLoadingVideoDevice = VideoDeviceList.Select(x=> x is null).ToReadOnlyReactiveProperty(true).AddTo(Disposables);

			IsLoadingAudioDevice = AudioDeviceList.Select(x => x is null).ToReadOnlyReactiveProperty(true).AddTo(Disposables);

			IsLoadingVideoReso = VideoDetailsList.Select(x => x is null).ToReadOnlyReactiveProperty(true).AddTo(Disposables);

			IsLoadingAudioDetails = AudioDetailsList.Select(x => x is null).ToReadOnlyReactiveProperty(true).AddTo(Disposables);


			if (json_string is not null)
			{
				PrevData = System.Text.Json.JsonSerializer.Deserialize<JsonData>(json_string);

			}

			Task.Run(async () => {
				LoadVideoTask = DeviceReader.LoadVideoDeviceList();
				LoadAudioTask = DeviceReader.LoadAudioDeviceList();
				VideoDeviceList.Value = await LoadVideoTask;
				if(PrevData is not null)
				{
					var t = VideoDeviceList.Value.FirstOrDefault(x => x.Name.Equals(PrevData.VideoName) && x.Path.Equals(PrevData.VideoPath));
					if(t is not null)VideoDevice.Value = t;
				}
			});






		}

		async Task VideoResoChange(NameInfo? name_info,int old_count)
		{
			await LoadItems(name_info, old_count, () => video_details_mutex_count, VideoDetailsList, VideoDetailsMap,
				async (prev) => {
					return await DeviceReader.LoadPinInfoList(MediaType.Video, prev.Name, prev.Duplicate);
				}, lst => {
					if (lst.FirstOrDefault(x => x.VideoInfoList.Count != 0) is PinInfo video_pin)
					{
						return video_pin.VideoInfoList;
					}
					return new List<VideoInfo>();
				}
			);

			if (PrevData is not null && VideoDetailsList.Value is not null)
			{
				var t = VideoDetailsList.Value.FirstOrDefault(x => x.Width == PrevData.Width && x.Height == PrevData.Height);
				if (t is not null) VideoDetail.Value = t;
			}
		}

		async Task AudioListChenge(NameInfo? name_info, int old_count)
		{
			await LoadItems(name_info,old_count,()=>video_details_mutex_count,AudioDeviceList,AudioListMap,
				async (prev) => {
					var lst = new List<NameInfo>(await LoadAudioTask);
					var pins = await VideoDetailsMap[prev];
					if (pins.FirstOrDefault(x => x.AudioInfoList.Count != 0) is PinInfo pin)
					{
						lst.Add(prev);

						if (!AudioDetailsMap.ContainsKey(prev))
						{
							AudioDetailsMap.Add(prev, VideoDetailsMap[prev]);
						}
					}
					return lst;
				}, x =>x
			);

			if (PrevData is not null && AudioDeviceList.Value is not null)
			{
				var t = AudioDeviceList.Value.FirstOrDefault(x => x.Name.Equals(PrevData.AudioName) && x.Path.Equals(PrevData.AudioPath));
				if (t is not null) AudioDevice.Value = t;
			}

		}

		async Task LoadItems<TPrev,TMiddle,TNext>(TPrev? prev,int old_count,Func<int> current_count,ReactiveProperty<TNext?> next_prop,Dictionary<TPrev,Task<TMiddle>> map,Func<TPrev,Task<TMiddle>> loadprocess,Func<TMiddle,TNext> selectprocess) 
			where TNext: class,new()
			where TPrev : class
		{
			next_prop.Value = null;

			if (prev is null)
			{
				next_prop.Value = new();
				return;
			}
			if (!map.ContainsKey(prev))
			{
				map.Add(prev,Task.Run(()=>loadprocess(prev)));
			}
			var ans = await map[prev];
			if (old_count != current_count()) return;
			next_prop.Value = selectprocess(ans);
		}

		public async Task<string> GetJsonString()
		{
			var data = new JsonData();
			data.VideoName = VideoDevice.Value.Name;
			data.VideoPath = VideoDevice.Value.Path;
			data.VideoPinName = (await VideoDetailsMap[VideoDevice.Value]).FirstOrDefault(x=>x.VideoInfoList.Count!=0)?.Name;
			data.Width = VideoDetail.Value.Width;
			data.Height = VideoDetail.Value.Height;
			data.VideoRotate = VideoRotate.Value;

			data.AudioName = AudioDevice.Value.Name;
			data.AudioPath = AudioDevice.Value.Path;
			data.AudioPinName = (await AudioDetailsMap[AudioDevice.Value]).FirstOrDefault(x => x.AudioInfoList.Count != 0)?.Name;
			data.AudioSampleRate = AudioSampleRate.Value;
			data.AudioType = AudioType.Value;

			return System.Text.Json.JsonSerializer.Serialize(data);

		}


	}

	/// <summary>
	/// An empty page that can be used on its own or navigated to within a Frame.
	/// </summary>
	public sealed partial class DeviceSettingPage : Page,IDisposable
	{
		public ObservableCollection<NameInfo> VideoDeviceList = new ();
		public ObservableCollection<VideoInfo> VideoResoList = new ();
		public ObservableCollection<NameInfo> AudioDeviceList = new ();
		public ObservableCollection<int> AudioSampleList = new ();
		public ObservableCollection<int> AudioTypeList = new ();

		DeviceSettingModel model;
		DeviceSettingModel.JsonData PrevData;
		void UpdateList<T>(ObservableCollection<T> target,IList<T>? source)
		{
			DispatcherQueue.TryEnqueue(() => {
				target.Clear();
				if (source is null) return;
				foreach(var i in source)
				{
					target.Add(i);
				}
			});

		}

		void UpdateIsLoading()
		{
			DispatcherQueue.TryEnqueue(() => {
				VideoNameComboBox.IsLoading = model.IsLoadingVideoDevice.CurrentValue;
				AudioNameComboBox.IsLoading = model.IsLoadingAudioDevice.CurrentValue;
				VideoResoComboBox.IsLoading = model.IsLoadingVideoReso.CurrentValue;
				SampleRateComboBox.IsLoading = model.IsLoadingAudioDetails.CurrentValue;
				AudioTypeComboBox.IsLoading = model.IsLoadingAudioDetails.CurrentValue;
			});

		}

		public DeviceSettingPage()
		{
			this.InitializeComponent();

			string? data = null;
			if (File.Exists(@"C:\Users\N7742\source\repos\QBS(WinUI)\resource\device_data.txt"))
			{
				data = File.ReadAllText(@"C:\Users\N7742\source\repos\QBS(WinUI)\resource\device_data.txt");
				PrevData = System.Text.Json.JsonSerializer.Deserialize<DeviceSettingModel.JsonData>(data);
			}

			model = new DeviceSettingModel(data);

			model.VideoDeviceList.Subscribe(lst => { UpdateList(VideoDeviceList, lst);
				if(PrevData is not null)
				{
					DispatcherQueue.TryEnqueue(() => {
						VideoNameComboBox.SelectedItem = model.VideoDevice.Value;
					});
				}
			
			}).AddTo(model.Disposables);
			model.AudioDeviceList.Subscribe(lst => { UpdateList(AudioDeviceList, lst);
				if (PrevData is not null)
				{
					DispatcherQueue.TryEnqueue(() => {
						AudioNameComboBox.SelectedItem = model.AudioDevice.Value;
					});
				}

			}).AddTo(model.Disposables);
			model.VideoDetailsList.Subscribe(lst => {UpdateList(VideoResoList, lst);
				if (PrevData is not null)
				{
					DispatcherQueue.TryEnqueue(() => {
						VideoResoComboBox.SelectedItem = model.VideoDetail.Value;
					});
				}

			}).AddTo(model.Disposables);
			model.AudioSampleRateList.Subscribe(lst => UpdateList(AudioSampleList, lst)).AddTo(model.Disposables);
			model.AudioTypeList.Subscribe(lst => UpdateList(AudioTypeList, lst)).AddTo(model.Disposables);

			model.IsLoadingVideoDevice.Subscribe(x=>UpdateIsLoading()).AddTo(model.Disposables);
			model.IsLoadingAudioDevice.Subscribe(x=>UpdateIsLoading()).AddTo(model.Disposables);
			model.IsLoadingVideoReso.Subscribe(x=>UpdateIsLoading()).AddTo(model.Disposables);
			model.IsLoadingAudioDetails.Subscribe(x=>UpdateIsLoading()).AddTo(model.Disposables);
		/*	model.VideoDevice.Subscribe(x => {
				DispatcherQueue.TryEnqueue(() => { VideoNameComboBox.SelectedItem = x; });
			});
			model.AudioDevice.Subscribe(x => {
				DispatcherQueue.TryEnqueue(() => { AudioNameComboBox.SelectedItem = x; });
			});
			model.VideoDetail.Subscribe(x => {
				DispatcherQueue.TryEnqueue(() => { VideoResoComboBox.SelectedItem = x; });
			});*/
		}

		static public string GetDeviceNameText(NameInfo nameInfo)
		{
			return nameInfo.Name;
		}

		static public string GetVideoInfoText(int w,int h,double f)
		{
			return $"{w}x{h} {(int)Math.Round(f)}fps";
		}
		
		private void Page_Loaded(object sender, RoutedEventArgs e)
		{
			
		}

		private void VideoNameComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			var s = sender as LoadingComboBox;
			model.VideoDevice.Value = (NameInfo)s.SelectedItem;
		}
		

		public void Dispose()
		{

		}

		private void Page_Unloaded(object sender, RoutedEventArgs e)
		{

			//string data = model.GetJsonString().Result;
			//File.WriteAllText(@"C:\Users\N7742\source\repos\QBS(WinUI)\resource\device_data.txt",data);
			model.Dispose();
		}

		private void AudioNameComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			var s = sender as LoadingComboBox;
			model.AudioDevice.Value = (NameInfo)s.SelectedItem;
		}

		private void ComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{

		}

		private void SamplingComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{

		}



		private void SampleRateComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			var s = sender as LoadingComboBox;
			model.AudioSampleRate.Value = s.SelectedItem is null?-1:(int)s.SelectedItem;
		}

		private void AudioTypeComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			var s = sender as LoadingComboBox;
			model.AudioType.Value = (int)s.SelectedItem;
		}

		private void VideoResoComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			var s = sender as LoadingComboBox;
			model.VideoDetail.Value = (VideoInfo)s.SelectedItem;
		}

		private void RotateComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			var s = sender as ComboBox;
			model.VideoRotate.Value = Rotate.D0;//s.SelectedItem is null ? -1 : (int)s.SelectedItem;
		}
	}
}


/*


*/
